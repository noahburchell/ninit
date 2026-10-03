#include "ninit.h"
#include "boot.h"
#include "cgroup.h"
#include "command.h"
#include "control.h"
#include "drain.h"
#include "fail.h"
#include "logging.h"
#include "ngraph.h"
#include "pidmap.h"
#include "reap.h"
#include "signals.h"
#include "state.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STALL_MS	10000
#define DEGRADED_POLL_MS 200
#define SIGNAL_POLL_MS	200

const void *map;
uint32_t n_svc;
uint8_t *state;
uint8_t *up;
uint8_t *want;
uint16_t *unmet;
struct run *runs;
uint32_t *live, n_live;
uint32_t *queue;
uint32_t *rqueue;
uint8_t *released;
uint32_t n_active, n_done, n_pending, n_up;
uint32_t drain_left;
uint32_t drain_rotor;
int shutting_down;
int null_fd = -1;
static int boot_reported;
static long long boot_t0;

long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void check_boot_done(void)
{
	uint32_t k;

	if (boot_reported || shutting_down || !n_svc || n_active)
		return;
	for (k = 0; k < n_live; k++)
		if (runs[live[k]].restart_at || runs[live[k]].stale_pid)
			return;
	boot_reported = 1;
	fail_summary(map, state);
	if (n_pending)
		log_warn("boot: %u service%s never started", n_pending, n_pending == 1 ? "" : "s");
	log_note("boot: %u of %u services up in %lld ms", n_done, n_svc, now_ms() - boot_t0);
}

static void start_graph(void)
{
	const struct ng_hdr *h = map;
	uint32_t i;

	n_svc = h->n_svc;
	n_pending = n_svc;
	state = calloc(n_svc, sizeof(*state));
	up = calloc(n_svc, sizeof(*up));
	want = calloc(n_svc, sizeof(*want));
	released = calloc(n_svc, sizeof(*released));
	rqueue = calloc(n_svc, sizeof(*rqueue));
	unmet = calloc(n_svc, sizeof(*unmet));
	runs = calloc(n_svc, sizeof(*runs));
	live = calloc(n_svc, sizeof(*live));
	queue = calloc(n_svc, sizeof(*queue));

	for (pid_mask = 64; pid_mask < 4 * n_svc; pid_mask *= 2)
		;
	pid_key = calloc(pid_mask, sizeof(*pid_key));
	pid_val = calloc(pid_mask, sizeof(*pid_val));
	pid_mask--;
	if (!pid_key || !pid_val) {
		free(pid_key);
		free(pid_val);
		pid_key = NULL;
		pid_val = NULL;
	}

	if (!state || !up || !want || !released || !rqueue || !unmet || !runs ||
	    !live  || !queue) {
		n_svc = 0;
		n_pending = 0;
		fail_emergency_shell("boot: out of memory before starting any service");
		return;
	}
	for (i = 0; i < n_svc; i++) {
		unmet[i] = ng_svcs(map)[i].unmet;
		runs[i].out_fd = -1;
		runs[i].ntf_fd = -1;
	}

	log_note("boot: %u services, %u roots", n_svc, h->n_roots);
	for (i = 0; i < h->n_roots; i++) {
		if (ng_svcs(map)[i].type == NG_TYPE_TARGET) {
			log_done("%s", ng_name(map, i));
			complete(i);
		} else {
			svc_try_start(i);
		}
	}
}

int main(int argc, char **argv)
{
	static struct pollfd sig_only[1];
	const char *graph = NG_DEFAULT_FILE, *why;
	struct pollfd *pfds;
	uint32_t *pf_idx;
	uint8_t *pf_kind;
	int placeholder, degraded;
	long long quiet_since;

	if (getpid() != 1) {
		log_err("ninit must run as pid 1");
		return 1;
	}

	log_init();
	ensure_stdio();
	{
		int f0 = fcntl(0, F_GETFL), f1 = fcntl(1, F_GETFL);

		placeholder = f0 >= 0 && (f0 & O_ACCMODE) == O_RDONLY &&
			      f1 >= 0 && (f1 & O_ACCMODE) == O_RDONLY;
	}
	umask(022);
	(void)!chdir("/");
	setup_signals();
	mount_api_fs();
	seed_dev();
	cgroup_init();

	// keep it off the oom victim list
	ninit_oom_score_adj("-1000\n", 6);

	null_fd = open("/dev/null", O_RDWR | O_CLOEXEC | O_NOCTTY);
	if (null_fd >= 0 && placeholder) {
		dup2(null_fd, 0);
		dup2(null_fd, 1);
		dup2(null_fd, 2);
	}
	log_reopen_console();
	raise_nofile();
	load_locale();
	print_welcome();
	ctl_init();
	log_feed(ctl_feed);
	boot_t0 = now_ms();

	// the kernel passes unknown key=value boot parameters to init
	why = getenv("ninit_graph");
	if (why && *why == '/')
		graph = why;
	if (argc > 1 && argv[1][0] == '/')
		graph = argv[1];
	map = load_graph(graph, &why);
	if (map) {
		start_graph();
	} else {
		char msg[256];

		snprintf(msg, sizeof(msg), "depgraph: %s: %s", graph, why);
		fail_emergency_shell(msg);
	}

	pfds = calloc(4 + CTL_MAX + 2 * (size_t)n_svc, sizeof(*pfds));
	pf_idx = calloc(4 + CTL_MAX + 2 * (size_t)n_svc, sizeof(*pf_idx));
	pf_kind = calloc(4 + CTL_MAX + 2 * (size_t)n_svc, sizeof(*pf_kind));
	degraded = !pfds || !pf_idx || !pf_kind;
	if (degraded) {
		free(pfds);
		free(pf_idx);
		free(pf_kind);
		pfds = sig_only;
		pf_idx = NULL;
		pf_kind = NULL;
		fail_emergency_shell("boot: out of memory building the poll set");
	}

	quiet_since = now_ms();
	for (;;) {
		long long wait, due;
		nfds_t nfds = 1, k;
		uint32_t start;
		int rc;

		log_batch_begin();
		drain_left = DRAIN_BUDGET;
		fire_restarts();
		check_timeouts();
		ctl_tick();
		for (int c = n_ctl; c-- > 0; )
			if (ctl_conn[c].out_at < ctl_conn[c].out_len ||
			    ctl_conn[c].listing || ctl_conn[c].done)
				ctl_pump(&ctl_conn[c]);
		check_boot_done();
		fail_emergency_tick();

		if (sfd < 0)
			poll_signals();
		if (degraded) {
			drain_all();
			if (fail_emergency_fd() >= 0)
				fail_emergency_report();
		}

		pfds[0].fd = sfd;
		pfds[0].events = POLLIN;
		start = n_live && live_has(drain_rotor) ? runs[drain_rotor].live_pos : 0;
		for (k = 0; !degraded && k < n_live; k++, start++) {
			uint32_t i;

			if (start >= n_live)
				start = 0;
			i = live[start];

			if (runs[i].out_fd >= 0) {
				pfds[nfds].fd = runs[i].out_fd;
				pfds[nfds].events = POLLIN;
				pf_idx[nfds] = i;
				pf_kind[nfds++] = 0;
			}
			if (runs[i].ntf_fd >= 0) {
				pfds[nfds].fd = runs[i].ntf_fd;
				pfds[nfds].events = POLLIN;
				pf_idx[nfds] = i;
				pf_kind[nfds++] = runs[i].ntf_exec ? 2 : 1;
			}
		}

		if (!degraded && fail_emergency_fd() >= 0) {
			pfds[nfds].fd = fail_emergency_fd();
			pfds[nfds].events = POLLIN;
			pf_idx[nfds] = 0;
			pf_kind[nfds++] = 3;
		}

		if (!degraded && ctl_lfd >= 0) {
			pfds[nfds].fd = ctl_lfd;
			pfds[nfds].events = POLLIN;
			pf_idx[nfds] = 0;
			pf_kind[nfds++] = 4;
			for (int c = 0; c < n_ctl; c++) {
				struct ctl *cc = &ctl_conn[c];

				pfds[nfds].fd = cc->fd;
				pfds[nfds].events = POLLIN;
				// a watcher that has caught up waits for new lines
				if (cc->out_at < cc->out_len ||
				    (cc->listing && (cc->listing != 2 || cc->log_at < log_end())))
					pfds[nfds].events |= POLLOUT;
				pf_idx[nfds] = (uint32_t)c;
				pf_kind[nfds++] = 5;
			}
		}

		if (!degraded && log_pending_fd() >= 0) {
			pfds[nfds].fd = log_pending_fd();
			pfds[nfds].events = POLLOUT;
			pf_idx[nfds] = 0;
			pf_kind[nfds++] = 6;
		}

		wait = n_active && !shutting_down ? STALL_MS : -1;
		due = restarts_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		due = starts_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		due = fail_emergency_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		due = ctl_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		if (degraded && (wait < 0 || wait > DEGRADED_POLL_MS))
			wait = DEGRADED_POLL_MS;
		if (sfd < 0 && (wait < 0 || wait > SIGNAL_POLL_MS))
			wait = SIGNAL_POLL_MS;

		rc = poll(pfds, nfds, (int)wait);
		if (rc < 0) {
			if (errno != EINTR) {
				log_err("poll: %s", strerror(errno));
				poll(NULL, 0, 1000);
			}
			continue;
		}
		if (rc == 0) {
			long long t = now_ms();

			if (t - quiet_since >= STALL_MS) {
				quiet_since = t;
				report_stalls();
			}
			continue;
		}
		quiet_since = now_ms();

		if (pfds[0].revents)
			handle_signals();

		for (k = 1; k < nfds; k++) {
			uint32_t i = pf_idx[k];

			if (!pfds[k].revents)
				continue;
			if (pf_kind[k] == 3) {
				fail_emergency_report();
				continue;
			}
			if (pf_kind[k] == 4) {
				ctl_accept();
				continue;
			}
			if (pf_kind[k] == 6) {
				log_flush();
				continue;
			}
			if (pf_kind[k] == 5) {
				// a reply may have closed and reshuffled the slots
				if (i >= (uint32_t)n_ctl || ctl_conn[i].fd != pfds[k].fd)
					continue;
				if (pfds[k].revents & (POLLOUT | POLLERR | POLLHUP))
					ctl_pump(&ctl_conn[i]);
				if (i < (uint32_t)n_ctl && ctl_conn[i].fd == pfds[k].fd &&
				    (pfds[k].revents & (POLLIN | POLLERR | POLLHUP)))
					ctl_read(&ctl_conn[i]);
				continue;
			}
			if (pf_kind[k] == 2)
				drain_exec(i);
			else if (pf_kind[k])
				drain_notify(i, DRAIN_POLL_CHUNKS);
			else
				drain_out(i, DRAIN_POLL_CHUNKS, 1);
			if (live_has(i))
				maybe_free(i);
		}
	}
}
