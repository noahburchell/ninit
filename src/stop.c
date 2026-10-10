#include "stop.h"
#include "control.h"
#include "drain.h"
#include "logging.h"
#include "ngraph.h"
#include "ninit.h"
#include "reap.h"
#include "signals.h"
#include "state.h"
#include "shutdown.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TERM_GRACE_MS	5000
#define STOP_TOTAL_MS	30000
#define PF_KTHREAD	0x00200000ul

static int userspace_left(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int found = 0;

	if (!d)
		return kill(-1, 0) == 0;

	while ((e = readdir(d)) != NULL) {
		char path[64], buf[512], *p;
		long pid;
		int fd, f;
		ssize_t k;

		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		pid = strtol(e->d_name, &p, 10);
		if (*p || pid <= 1)
			continue;
		snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		k = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (k <= 0)
			continue;
		buf[k] = '\0';

		p = strrchr(buf, ')');
		if (!p)
			continue;
		p++;
		for (f = 0; f < 6 && *p; f++) {
			while (*p == ' ')
				p++;
			while (*p && *p != ' ')
				p++;
		}
		while (*p == ' ')
			p++;
		if (strtoul(p, NULL, 10) & PF_KTHREAD)
			continue;

		found = 1;
		break;
	}

	closedir(d);
	return found;
}

static void wait_children(long long grace)
{
	long long deadline = now_ms() + grace;

	for (;;) {
		struct pollfd p = { .fd = sfd, .events = POLLIN };
		struct signalfd_siginfo si;
		long long left;

		log_batch_begin();
		drain_left = DRAIN_BUDGET;
		reap();
		drain_output();
		if (!userspace_left())
			return;
		left = deadline - now_ms();
		if (left <= 0)
			return;
		if (left > SHUTDOWN_DRAIN_MS)
			left = SHUTDOWN_DRAIN_MS;
		poll(&p, 1, (int)left);
		while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si))
			;
	}
}

static int build_prereqs(uint32_t **poff, uint32_t **pidx)
{
	const uint32_t *roff = ng_rdep_off(map), *ridx = ng_rdep_idx(map);
	uint32_t m = ((const struct ng_hdr *)map)->n_edges;
	uint32_t *off = calloc((size_t)n_svc + 1, sizeof(*off));
	uint32_t *idx = calloc(m ? m : 1, sizeof(*idx));
	uint32_t *fill = calloc(n_svc, sizeof(*fill));
	uint32_t i, k;

	if (!off || !idx || !fill) {
		free(off);
		free(idx);
		free(fill);
		return -1;
	}
	for (i = 0; i < n_svc; i++)
		off[i + 1] = off[i] + ng_svcs(map)[i].unmet;
	// ng_verify() proved this, but the graph is a live mapping
	if (off[n_svc] != m) {
		free(off);
		free(idx);
		free(fill);
		return -1;
	}
	for (i = 0; i < n_svc; i++)
		for (k = roff[i]; k < roff[i + 1]; k++) {
			uint32_t d = ridx[k];

			idx[off[d] + fill[d]++] = i;
		}
	free(fill);
	*poff = off;
	*pidx = idx;
	return 0;
}

static void stop_ordered(void)
{
	const uint32_t *roff = ng_rdep_off(map);
	uint32_t *off = NULL, *idx = NULL, *waitc = NULL;
	long long *stop_at = NULL, deadline;
	uint8_t *st = NULL;
	uint32_t i, k, left = 0, live_svc = 0;

	if (!n_svc)
		goto out;
	if (build_prereqs(&off, &idx) < 0) {
		log_warn("shutdown: out of memory, stopping every service at once instead");
		goto out;
	}
	waitc = calloc(n_svc, sizeof(*waitc));
	stop_at = calloc(n_svc, sizeof(*stop_at));
	st = calloc(n_svc, sizeof(*st));
	if (!waitc || !stop_at || !st) {
		log_warn("shutdown: out of memory, stopping every service at once instead");
		goto out;
	}

	for (i = 0; i < n_svc; i++) {
		waitc[i] = roff[i + 1] - roff[i];
		if (!svc_gone(i))
			live_svc++;
	}
	if (!live_svc)
		goto out;
	left = n_svc;

	log_note("shutdown: stopping %u service%s in dependency order", live_svc,
		 live_svc == 1 ? "" : "s");

	deadline = now_ms() + STOP_TOTAL_MS;
	for (;;) {
		struct pollfd p = { .fd = sfd, .events = POLLIN };
		struct signalfd_siginfo si;
		long long now = now_ms(), next = deadline;

		for (i = 0; i < n_svc; i++) {
			if (st[i] == 2 || waitc[i])
				continue;
			if (!st[i]) {
				if (svc_gone(i))
					continue;	// settles below
				signal_group(i, SIGTERM);
				st[i] = 1;
				stop_at[i] = now + ng_stop_ms(map, i);
			}
			if (st[i] == 1 && now >= stop_at[i]) {
				log_warn("%s: still running %u ms after SIGTERM, killing it",
					 ng_name(map, i), ng_stop_ms(map, i));
				signal_group(i, SIGKILL);
				st[i] = 3;
				stop_at[i] = now + KILL_GRACE_MS;
			}
			if (st[i] && stop_at[i] < next)
				next = stop_at[i];
		}

		log_batch_begin();
		drain_left = DRAIN_BUDGET;
		reap();
		drain_output();

		for (i = 0; i < n_svc; i++) {
			if (st[i] == 2 || waitc[i] || !svc_gone(i))
				continue;
			st[i] = 2;
			left--;
			for (k = off[i]; k < off[i + 1]; k++)
				if (waitc[idx[k]])
					waitc[idx[k]]--;
		}
		if (!left)
			break;

		now = now_ms();
		if (now >= deadline) {
			uint32_t stuck = 0;

			for (i = 0; i < n_svc; i++)
				stuck += !svc_gone(i);
			log_warn("shutdown: %u service%s did not stop in order", stuck,
				 stuck == 1 ? "" : "s");
			break;
		}
		if (next > now + SHUTDOWN_DRAIN_MS)
			next = now + SHUTDOWN_DRAIN_MS;
		if (next < now)
			next = now;
		if (sfd >= 0) {
			poll(&p, 1, (int)(next - now));
			while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si))
				;
		} else {
			poll(NULL, 0, (int)(next - now));
		}
	}
out:
	free(off);
	free(idx);
	free(waitc);
	free(stop_at);
	free(st);
}

void shutdown_system(int how, const char *what)
{
	shutting_down = 1;
	stop_ordered();
	log_note("%s: sending SIGTERM to all processes", what);
	kill(-1, SIGTERM);
	wait_children(TERM_GRACE_MS);
	if (userspace_left()) {
		log_warn("%s: sending SIGKILL to all processes", what);
		kill(-1, SIGKILL);
		wait_children(KILL_GRACE_MS);
	}
	log_note("%s: saving the clock and disabling swap", what);
	save_hwclock();
	stop_swap();

	// an unlinked socket that is still open keeps /run from going read-only
	for (int c = 0; c < n_ctl; c++)
		close(ctl_conn[c].fd);
	n_ctl = 0;
	if (ctl_lfd >= 0) {
		close(ctl_lfd);
		ctl_lfd = -1;
		unlink(NINIT_CTL_SOCK);
	}
	log_note("%s: syncing and remounting read-only", what);
	sync();
	remount_ro();
	sync();
	log_note("%s: now", what);
	reboot(how);
	log_err("%s: reboot(): %s", what, strerror(errno));
	for (;;)
		pause();
}
