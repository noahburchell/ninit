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
#include "../help/time.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STALL_MS	 10000
#define DEGRADED_POLL_MS 200
#define SIGNAL_POLL_MS	 200

// what a polled descriptor after the signalfd belongs to
enum pf_kind {
	PF_OUT,
	PF_NOTIFY,
	PF_EXEC,
	PF_SHELL,
	PF_LISTEN,
	PF_CTL,
	PF_CONSOLE,
};

const void* map;
uint32_t n_svc;
uint8_t* state;
uint8_t* up;
uint8_t* want;
uint16_t* unmet;
struct run* runs;
uint32_t* live;
uint32_t n_live;
uint32_t* queue;
uint32_t* rqueue;
uint8_t* released;
uint32_t n_active, n_done, n_pending, n_up;
uint32_t drain_left;
uint32_t drain_rotor;
int shutting_down;
int null_fd = -1;
long long boot_ms = -1;
static long long boot_t0;

long long now_ms(void) {
	return mono_ms();
}

// the soonest restart delay, start timeout or control deadline among the live services, -1 for none
static long long live_due(long long now) {
	long long best = -1;
	uint32_t k;

	for(k = 0; k < n_live; k++) {
		const struct run* r = &runs[live[k]];

		if(r->restart_at) {
			best = ms_sooner(best, ms_until(r->restart_at, now));
		}

		if(r->starting && !r->timedout) {
			best = ms_sooner(best, ms_until(r->started + ng_start_ms(map, live[k]), now));
		}

		if(r->op != SVC_OP_NONE) {
			best = ms_sooner(best, ms_until(r->op_at, now));
		}
	}

	return best;
}

static void check_boot_done(void) {
	uint32_t k;

	if(boot_ms >= 0 || shutting_down || !n_svc || n_active) {
		return;
	}

	for(k = 0; k < n_live; k++) {
		if(runs[live[k]].restart_at || runs[live[k]].stale_pid) {
			return;
		}
	}

	boot_ms = now_ms() - boot_t0;
	fail_summary(map, state);
	if(n_pending) {
		log_warn("boot: %u service%s never started", n_pending, plural(n_pending));
	}

	log_note("boot: %u of %u services up in %lld ms", n_done, n_svc, boot_ms);
}

static void start_graph(void) {
	const struct ng_hdr* h = map;
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

	for(pid_mask = 64; pid_mask < 4 * n_svc; pid_mask *= 2)
		;
	pid_key = calloc(pid_mask, sizeof(*pid_key));
	pid_val = calloc(pid_mask, sizeof(*pid_val));
	pid_mask--;
	if(!pid_key || !pid_val) {
		free(pid_key);
		free(pid_val);
		pid_key = NULL;
		pid_val = NULL;
	}

	if(!state || !up || !want || !released || !rqueue || !unmet || !runs
	   || !live || !queue) {
		n_svc = 0;
		n_pending = 0;
		fail_emergency_shell("boot: out of memory before starting any service");
		return;
	}

	for(i = 0; i < n_svc; i++) {
		unmet[i] = ng_svcs(map)[i].unmet;
		runs[i].out_fd = -1;
		runs[i].ntf_fd = -1;
	}

	log_note("boot: %u services, %u roots", n_svc, h->n_roots);
	for(i = 0; i < h->n_roots; i++) {
		if(ng_svcs(map)[i].type == NG_TYPE_TARGET) {
			log_done("%s", ng_name(map, i));
			complete(i);
		} else {
			svc_try_start(i);
		}
	}
}

// the graph the last ninitctl init replaced boots in place of one that fails to load
static const void* load_previous(const char* graph, const char* why) {
	char old[4096], msg[sizeof(old) + 128];
	const void* m;

	if(snprintf(old, sizeof(old), "%s.old", graph) >= (int)sizeof(old)
	   || access(old, F_OK) < 0) {
		snprintf(msg, sizeof(msg), "depgraph: %s: %s", graph, why);
		fail_emergency_shell(msg);
		return NULL;
	}

	log_err("depgraph: %s: %s", graph, why);
	m = load_graph(old, &why);
	if(m) {
		log_warn("depgraph: loaded %s instead", old);
		return m;
	}

	snprintf(msg, sizeof(msg), "depgraph: %s: %s", old, why);
	fail_emergency_shell(msg);

	return NULL;
}

int main(int argc, char** argv) {
	static struct pollfd sig_only[1];
	const char* graph = NG_DEFAULT_FILE;
	const char* why;
	struct pollfd* pfds;
	uint32_t* pf_idx;
	uint8_t* pf_kind;
	int placeholder, degraded;
	long long quiet_since;

	if(getpid() != 1) {
		log_err("ninit must run as pid 1");
		return 1;
	}

	log_init();
	ensure_stdio();
	{
		int f0 = fcntl(0, F_GETFL), f1 = fcntl(1, F_GETFL);

		placeholder = f0 >= 0 && (f0 & O_ACCMODE) == O_RDONLY
			      && f1 >= 0 && (f1 & O_ACCMODE) == O_RDONLY;
	}

	umask(022);
	(void)!chdir("/");
	setup_signals();
	mount_api_fs();
	seed_dev();
	cgroup_init();

	// keep it off the oom victim list
	ninit_oom_score_adj(STR("-1000\n"));

	null_fd = open("/dev/null", O_RDWR | O_CLOEXEC | O_NOCTTY);
	if(null_fd >= 0 && placeholder) {
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
	if(why && *why == '/') {
		graph = why;
	}

	if(argc > 1 && argv[1][0] == '/') {
		graph = argv[1];
	}

	map = load_graph(graph, &why);
	if(!map) {
		map = load_previous(graph, why);
	}

	if(map) {
		start_graph();
	}

	pfds = calloc(4 + CTL_MAX + 2 * (size_t)n_svc, sizeof(*pfds));
	pf_idx = calloc(4 + CTL_MAX + 2 * (size_t)n_svc, sizeof(*pf_idx));
	pf_kind = calloc(4 + CTL_MAX + 2 * (size_t)n_svc, sizeof(*pf_kind));
	degraded = !pfds || !pf_idx || !pf_kind;
	if(degraded) {
		free(pfds);
		free(pf_idx);
		free(pf_kind);
		pfds = sig_only;
		pf_idx = NULL;
		pf_kind = NULL;
		fail_emergency_shell("boot: out of memory building the poll set");
	}

	quiet_since = now_ms();
	for(;;) {
		long long wait;
		nfds_t nfds = 1, k;
		uint32_t start;
		int rc, shell_fd, con_fd;

		log_batch_begin();
		drain_left = DRAIN_BUDGET;
		fire_restarts();
		check_timeouts();
		ctl_tick();
		log_tick();
		for(int c = n_ctl; c-- > 0;) {
			if(ctl_conn[c].out_at < ctl_conn[c].out_len
			   || ctl_conn[c].listing || ctl_conn[c].done) {
				ctl_pump(&ctl_conn[c]);
			}
		}

		check_boot_done();
		fail_emergency_tick();

		if(sfd < 0) {
			poll_signals();
		}

		shell_fd = fail_emergency_fd();
		con_fd = log_pending_fd();
		if(degraded) {
			drain_all();
			if(shell_fd >= 0) {
				fail_emergency_report();
			}
		}

		pfds[0].fd = sfd;
		pfds[0].events = POLLIN;
		start = n_live && live_has(drain_rotor) ? runs[drain_rotor].live_pos : 0;
		for(k = 0; !degraded && k < n_live; k++, start++) {
			uint32_t i;

			if(start >= n_live) {
				start = 0;
			}

			i = live[start];

			if(runs[i].out_fd >= 0) {
				pfds[nfds].fd = runs[i].out_fd;
				pfds[nfds].events = POLLIN;
				pf_idx[nfds] = i;
				pf_kind[nfds++] = PF_OUT;
			}

			if(runs[i].ntf_fd >= 0) {
				pfds[nfds].fd = runs[i].ntf_fd;
				pfds[nfds].events = POLLIN;
				pf_idx[nfds] = i;
				pf_kind[nfds++] = runs[i].ntf_exec ? PF_EXEC : PF_NOTIFY;
			}
		}

		if(!degraded && shell_fd >= 0) {
			pfds[nfds].fd = shell_fd;
			pfds[nfds].events = POLLIN;
			pf_idx[nfds] = 0;
			pf_kind[nfds++] = PF_SHELL;
		}

		if(!degraded && ctl_lfd >= 0) {
			pfds[nfds].fd = ctl_lfd;
			pfds[nfds].events = POLLIN;
			pf_idx[nfds] = 0;
			pf_kind[nfds++] = PF_LISTEN;
			for(int c = 0; c < n_ctl; c++) {
				struct ctl* cc = &ctl_conn[c];

				pfds[nfds].fd = cc->fd;
				pfds[nfds].events = POLLIN;
				// a watcher that has caught up waits for new lines
				if(cc->out_at < cc->out_len
				   || (cc->listing && (cc->listing != 2 || cc->log_at < log_end()))) {
					pfds[nfds].events |= POLLOUT;
				}

				pf_idx[nfds] = (uint32_t)c;
				pf_kind[nfds++] = PF_CTL;
			}
		}

		if(!degraded && con_fd >= 0) {
			pfds[nfds].fd = con_fd;
			pfds[nfds].events = POLLOUT;
			pf_idx[nfds] = 0;
			pf_kind[nfds++] = PF_CONSOLE;
		}

		wait = n_active && !shutting_down ? STALL_MS : -1;
		wait = ms_sooner(wait, live_due(now_ms()));
		wait = ms_sooner(wait, fail_emergency_due());
		wait = ms_sooner(wait, log_due());
		if(degraded) {
			wait = ms_sooner(wait, DEGRADED_POLL_MS);
		}

		if(sfd < 0) {
			wait = ms_sooner(wait, SIGNAL_POLL_MS);
		}

		rc = poll(pfds, nfds, (int)wait);
		if(rc < 0) {
			if(errno != EINTR) {
				log_err("poll: %s", strerror(errno));
				poll(NULL, 0, 1000);
			}

			continue;
		}

		if(rc == 0) {
			long long t = now_ms();

			if(t - quiet_since >= STALL_MS) {
				quiet_since = t;
				report_stalls();
			}

			continue;
		}

		quiet_since = now_ms();

		if(pfds[0].revents) {
			handle_signals();
		}

		for(k = 1; k < nfds; k++) {
			uint32_t i = pf_idx[k];

			if(!pfds[k].revents) {
				continue;
			}

			if(pf_kind[k] == PF_SHELL) {
				fail_emergency_report();
				continue;
			}

			if(pf_kind[k] == PF_LISTEN) {
				ctl_accept();
				continue;
			}

			if(pf_kind[k] == PF_CONSOLE) {
				log_flush();
				continue;
			}

			if(pf_kind[k] == PF_CTL) {
				// a reply may have closed and reshuffled the slots
				if(i >= (uint32_t)n_ctl || ctl_conn[i].fd != pfds[k].fd) {
					continue;
				}

				if(pfds[k].revents & (POLLOUT | POLLERR | POLLHUP)) {
					ctl_pump(&ctl_conn[i]);
				}

				if(i < (uint32_t)n_ctl && ctl_conn[i].fd == pfds[k].fd
				   && (pfds[k].revents & (POLLIN | POLLERR | POLLHUP))) {
					ctl_read(&ctl_conn[i]);
				}

				continue;
			}

			if(pf_kind[k] == PF_EXEC) {
				drain_exec(i);
			} else if(pf_kind[k] == PF_NOTIFY) {
				drain_notify(i, DRAIN_POLL_CHUNKS);
			} else {
				drain_out(i, DRAIN_POLL_CHUNKS, 1);
			}

			if(live_has(i)) {
				maybe_free(i);
			}
		}
	}
}
