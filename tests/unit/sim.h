#pragma once

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "tap.h"

// pid 1 runs here as a library, every process it would create or signal is simulated

#define SIM_PID0 3000000
#define SIM_MAX	 4096

struct sim_child {
	pid_t pid;
	int   out_w, ntf_w;
	int   alive;
	int   ignore_term;
};

static struct sim_child kids[SIM_MAX];
static int n_kids;
static int pend_w[4], n_pend_w;
static long long fake_ms = 1000000;

struct sim_exit {
	pid_t pid;
	int   status;
};

static struct sim_exit exits[SIM_MAX];
static int n_exits;
static int sim_fork_fail;

static int sim_clock_gettime(clockid_t id, struct timespec* ts) {
	(void)id;
	ts->tv_sec = fake_ms / 1000;
	ts->tv_nsec = (fake_ms % 1000) * 1000000;

	return 0;
}

static int sim_pipe2(int p[2], int flags) {
	int r = pipe2(p, flags);

	if(!r && n_pend_w < 4) {
		pend_w[n_pend_w++] = p[1];
	}

	return r;
}

static struct sim_child* kid_of(pid_t pid) {
	for(int k = 0; k < n_kids; k++) {
		if(kids[k].pid == pid) {
			return &kids[k];
		}
	}

	return NULL;
}

static pid_t sim_fork(void) {
	struct sim_child* c;

	if(sim_fork_fail || n_kids == SIM_MAX) {
		n_pend_w = 0;
		errno = EAGAIN;
		return -1;
	}

	c = &kids[n_kids++];
	memset(c, 0, sizeof(*c));
	c->pid = SIM_PID0 + n_kids;
	c->out_w = n_pend_w > 0 ? fcntl(pend_w[0], F_DUPFD_CLOEXEC, 3) : -1;
	c->ntf_w = n_pend_w > 1 ? fcntl(pend_w[1], F_DUPFD_CLOEXEC, 3) : -1;
	c->alive = 1;
	n_pend_w = 0;

	return c->pid;
}

static void kid_close(struct sim_child* c) {
	if(c->out_w >= 0) {
		close(c->out_w);
	}

	if(c->ntf_w >= 0) {
		close(c->ntf_w);
	}

	c->out_w = c->ntf_w = -1;
}

// the process dies with STATUS, its pipes close as the kernel would close them
static void kid_die(struct sim_child* c, int status) {
	if(!c->alive) {
		return;
	}

	c->alive = 0;
	kid_close(c);
	exits[n_exits].pid = c->pid;
	exits[n_exits].status = status;
	n_exits++;
}

static int sim_kill(pid_t pid, int sig) {
	pid_t who = pid < 0 ? -pid : pid;
	struct sim_child* c;

	// a real pid would reach a real process, that must never happen
	if(who <= SIM_PID0 || who > SIM_PID0 + SIM_MAX) {
		abort();
	}

	c = kid_of(who);
	if(!c || !c->alive) {
		errno = ESRCH;
		return -1;
	}

	if(sig == 0) {
		return 0;
	}

	if(sig == SIGTERM && c->ignore_term) {
		return 0;
	}

	kid_die(c, sig);

	return 0;
}

static pid_t sim_waitpid(pid_t pid, int* st, int flags) {
	(void)pid;
	(void)flags;
	if(n_exits) {
		pid_t p = exits[0].pid;

		*st = exits[0].status;
		memmove(exits, exits + 1, (size_t)--n_exits * sizeof(*exits));
		return p;
	}

	for(int k = 0; k < n_kids; k++) {
		if(kids[k].alive) {
			return 0;
		}
	}

	errno = ECHILD;

	return -1;
}

static const char* adjtime_path;

static int sim_open(const char* path, int flags, ...) {
	if(!strcmp(path, "/dev/null")) {
		return open(path, flags);
	}

	if(!strcmp(path, "/etc/adjtime") && adjtime_path) {
		return open(adjtime_path, flags);
	}

	errno = EACCES;

	return -1;
}

[[noreturn]] static void sim_forbidden(void) {
	abort();
}

static int sim_execve(const char* p, char* const* a, char* const* e) {
	(void)p;
	(void)a;
	(void)e;
	sim_forbidden();
}

static int sim_reboot(int how) {
	(void)how;
	sim_forbidden();
}

static void sim_sync(void) {
	sim_forbidden();
}

static int sim_mount(const char* a, const char* b, const char* c, unsigned long d, const void* e) {
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	sim_forbidden();
}

static int sim_umount2(const char* a, int b) {
	(void)a;
	(void)b;
	sim_forbidden();
}

static int sim_swapoff(const char* a) {
	(void)a;
	sim_forbidden();
}

#define clock_gettime sim_clock_gettime
#define pipe2	      sim_pipe2
#define fork	      sim_fork
#define kill	      sim_kill
#define waitpid	      sim_waitpid
#define open	      sim_open
#define execve	      sim_execve
#define reboot	      sim_reboot
#define sync	      sim_sync
#define mount	      sim_mount
#define umount2	      sim_umount2
#define swapoff	      sim_swapoff
#define main	      ninit_main
int ninit_main(int argc, char** argv);
// the forbidden shims make exported callers such as remount_ro noreturn here, never in ninit
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsuggest-attribute=noreturn"
#endif
#include "../../src/fail.c"
#include "../../src/boot.c"
#include "../../src/cgroup.c"
#include "../../src/command.c"
#include "../../src/control.c"
#include "../../src/drain.c"
#include "../../src/pidmap.c"
#include "../../src/reap.c"
#include "../../src/signals.c"
#include "../../src/spawn.c"
#include "../../src/state.c"
#include "../../src/stop.c"
#include "../../src/shutdown.c"
#include "../../src/ninit.c"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#undef clock_gettime
#undef pipe2
#undef fork
#undef kill
#undef waitpid
#undef open
#undef execve
#undef reboot
#undef sync
#undef mount
#undef umount2
#undef swapoff
#undef main

#include "gbuild.h"
#include "logcap.h"

// state is global in ninit.c, everything below drives the real functions

static struct gb_img sim_img;

static inline void sim_reset(void) {
	// a simulated child writes from this process, so a pipe ninit has closed must fail with EPIPE instead
	signal(SIGPIPE, SIG_IGN);
	for(int k = 0; k < n_kids; k++) {
		kid_close(&kids[k]);
	}

	n_kids = n_exits = n_pend_w = 0;
	while(n_ctl) {
		ctl_drop(&ctl_conn[n_ctl - 1]);
	}

	for(uint32_t i = 0; runs && i < n_svc; i++) {
		if(runs[i].out_fd >= 0) {
			close(runs[i].out_fd);
		}

		if(runs[i].ntf_fd >= 0) {
			close(runs[i].ntf_fd);
		}
	}

	free(state);
	free(up);
	free(want);
	free(released);
	free(rqueue);
	free(unmet);
	free(runs);
	free(live);
	free(queue);
	free(pid_key);
	free(pid_val);
	state = up = want = released = NULL;
	rqueue = queue = live = NULL;
	unmet = NULL;
	runs = NULL;
	pid_key = NULL;
	pid_val = NULL;
	n_live = q_head = q_tail = 0;
	draining = 0;
	n_active = n_done = n_pending = n_up = 0;
	boot_ms = -1;
	shutting_down = 0;
	n_ops = 0;
	cg_ok = 0;
	emerg_pid = -1;
	emerg_gone = 1;
	free(sim_img.map);
	sim_img.map = NULL;
}

static inline void sim_load(const struct gb_svc* sv, uint32_t n, const struct gb_edge* e, uint32_t m) {
	sim_reset();
	sim_img = gb_build(sv, n, e, m);
	if(ng_verify(sim_img.map, sim_img.len)) {
		abort();
	}

	map = sim_img.map;
	start_graph();
}

static inline struct sim_child* kid_svc(uint32_t i) {
	return runs[i].pid > 0 ? kid_of(runs[i].pid) : NULL;
}

static inline void sim_drain(void) {
	drain_left = DRAIN_BUDGET;
	drain_all();
}

// a daemon without notify is ready once its shell execs, which closes the exec pipe
static inline void sim_exec_all(void) {
	for(uint32_t i = 0; i < n_svc; i++) {
		struct sim_child* c;

		if(!runs[i].ntf_exec || !runs[i].starting || runs[i].pid <= 0) {
			continue;
		}

		c = kid_of(runs[i].pid);
		if(c && c->alive && c->ntf_w >= 0) {
			close(c->ntf_w);
			c->ntf_w = -1;
		}
	}
}

static inline void sim_step(void) {
	for(int round = 0; round < 4; round++) {
		sim_exec_all();
		log_batch_begin();
		fire_restarts();
		check_timeouts();
		ctl_tick();
		for(int c = n_ctl; c-- > 0;) {
			if(ctl_conn[c].out_at < ctl_conn[c].out_len || ctl_conn[c].listing
			   || ctl_conn[c].done) {
				ctl_pump(&ctl_conn[c]);
			}
		}

		check_boot_done();
		sim_drain();
		reap();
		sim_drain();
	}
}

static inline void sim_advance(long long ms) {
	while(ms > 0) {
		long long d = ms > 50 ? 50 : ms;

		fake_ms += d;
		ms -= d;
		sim_step();
	}
}

// the service became ready the way its type makes it ready
static inline void sim_ready(uint32_t i) {
	struct sim_child* c = kid_svc(i);

	if(!c) {
		return;
	}

	if(ng_svcs(map)[i].type == NG_TYPE_DAEMON) {
		if(ng_notify(map, i)) {
			(void)!write(c->ntf_w, "\n", 1);
		} else if(c->ntf_w >= 0) {
			close(c->ntf_w);
			c->ntf_w = -1;
		}
	} else {
		kid_die(c, 0);
	}

	sim_step();
}

static inline void sim_exit_svc(uint32_t i, int status) {
	struct sim_child* c = kid_svc(i);

	if(c) {
		kid_die(c, status);
	}

	sim_step();
}

static inline void sim_say(uint32_t i, const char* text) {
	struct sim_child* c = kid_svc(i);

	if(c && c->out_w >= 0) {
		(void)!write(c->out_w, text, strlen(text));
	}

	sim_step();
}

static inline uint32_t svc_by(const char* name) {
	return ctl_find(name);
}

// sends REQ as a control client, returns the reply once ninit has finished it
static inline const char* sim_ctl(const char* req) {
	static char reply[CTL_OUT * 4];
	int sv[2];
	struct ctl* c;
	char line[CTL_BUF];
	size_t at = 0;

	if(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) < 0 || n_ctl == CTL_MAX) {
		abort();
	}

	c = &ctl_conn[n_ctl++];
	memset(c, 0, sizeof(*c));
	c->fd = sv[0];
	c->svc = UINT32_MAX;
	snprintf(line, sizeof(line), "%s", req);
	ctl_cmd(c, line);
	ctl_pump(c);
	for(int k = 0; k < 400; k++) {
		ssize_t n = read(sv[1], reply + at, sizeof(reply) - 1 - at);

		if(n > 0) {
			at += (size_t)n;
			reply[at] = '\0';
			if(strstr(reply, "\n+ ") || strstr(reply, "\n- ") || !strncmp(reply, "+ ", 2)
			   || !strncmp(reply, "- ", 2)) {
				break;
			}

			continue;
		}

		if(n == 0) {
			break;
		}

		sim_advance(20);
	}

	reply[at] = '\0';
	close(sv[1]);
	sim_step();

	return reply;
}

static inline const char* st_of(const char* name) {
	return state_name(svc_by(name));
}

// counters ninit keeps alongside the state array must match it
static inline const char* invariants(void) {
	uint32_t a = 0, d = 0, p = 0, u = 0;

	for(uint32_t i = 0; i < n_svc; i++) {
		a += state[i] == NG_ST_RUNNING;
		d += state[i] == NG_ST_DONE;
		p += state[i] == NG_ST_PENDING;
		u += up[i] != 0;
		// a completed oneshot run again by start stays up for its dependents while it runs
		if(up[i] && state[i] != NG_ST_DONE && state[i] != NG_ST_RUNNING) {
			return "a service is up without being done";
		}

		if(runs[i].pid > 0) {
			struct sim_child* c = kid_of(runs[i].pid);
			bool stale;

			if(!c) {
				return "a service names a pid that was never forked";
			}

			if(find_pid(runs[i].pid, &stale) != i) {
				return "a running pid is missing from the pid table";
			}
		}
	}

	for(uint32_t k = 0; k < n_live; k++) {
		if(runs[live[k]].live_pos != k) {
			return "the live list and its positions disagree";
		}

		for(uint32_t j = k + 1; j < n_live; j++) {
			if(live[j] == live[k]) {
				return "a service is in the live list twice";
			}
		}
	}

	if(a != n_active) {
		return "n_active does not count the starting services";
	}

	if(d != n_done) {
		return "n_done does not count the done services";
	}

	if(p != n_pending) {
		return "n_pending does not count the pending services";
	}

	if(u != n_up) {
		return "n_up does not count the up services";
	}

	return NULL;
}

static inline int inv_ok(const char* what) {
	const char* why = invariants();

	if(why) {
		ok(0, "%s: %s", what, why);
		return 0;
	}

	return 1;
}
