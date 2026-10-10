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

#define SIM_PID0	3000000
#define SIM_MAX		4096

struct sim_child {
	pid_t pid;
	int out_w, ntf_w;
	int alive;
	int ignore_term;
};

static struct sim_child kids[SIM_MAX];
static int n_kids;
static int pend_w[4], n_pend_w;
static long long fake_ms = 1000000;

struct sim_exit {
	pid_t pid;
	int status;
};

static struct sim_exit exits[SIM_MAX];
static int n_exits;
static int sim_fork_fail;

static int sim_clock_gettime(clockid_t id, struct timespec *ts)
{
	(void)id;
	ts->tv_sec = fake_ms / 1000;
	ts->tv_nsec = (fake_ms % 1000) * 1000000;
	return 0;
}

static int sim_pipe2(int p[2], int flags)
{
	int r = pipe2(p, flags);

	if (!r && n_pend_w < 4)
		pend_w[n_pend_w++] = p[1];
	return r;
}

static struct sim_child *kid_of(pid_t pid)
{
	for (int k = 0; k < n_kids; k++)
		if (kids[k].pid == pid)
			return &kids[k];
	return NULL;
}

static pid_t sim_fork(void)
{
	struct sim_child *c;

	if (sim_fork_fail || n_kids == SIM_MAX) {
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

static void kid_close(struct sim_child *c)
{
	if (c->out_w >= 0)
		close(c->out_w);
	if (c->ntf_w >= 0)
		close(c->ntf_w);
	c->out_w = c->ntf_w = -1;
}

// the process dies with STATUS, its pipes close as the kernel would close them
static void kid_die(struct sim_child *c, int status)
{
	if (!c->alive)
		return;
	c->alive = 0;
	kid_close(c);
	exits[n_exits].pid = c->pid;
	exits[n_exits].status = status;
	n_exits++;
}

static int sim_kill(pid_t pid, int sig)
{
	pid_t who = pid < 0 ? -pid : pid;
	struct sim_child *c;

	// a real pid would reach a real process, that must never happen
	if (who <= SIM_PID0 || who > SIM_PID0 + SIM_MAX)
		abort();
	c = kid_of(who);
	if (!c || !c->alive) {
		errno = ESRCH;
		return -1;
	}
	if (sig == 0)
		return 0;
	if (sig == SIGTERM && c->ignore_term)
		return 0;
	kid_die(c, sig);
	return 0;
}

static pid_t sim_waitpid(pid_t pid, int *st, int flags)
{
	(void)pid;
	(void)flags;
	if (n_exits) {
		pid_t p = exits[0].pid;

		*st = exits[0].status;
		memmove(exits, exits + 1, (size_t)--n_exits * sizeof(*exits));
		return p;
	}
	for (int k = 0; k < n_kids; k++)
		if (kids[k].alive)
			return 0;
	errno = ECHILD;
	return -1;
}

static const char *adjtime_path;

static int sim_open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/dev/null"))
		return open(path, flags);
	if (!strcmp(path, "/etc/adjtime") && adjtime_path)
		return open(adjtime_path, flags);
	errno = EACCES;
	return -1;
}

[[noreturn]] static void sim_forbidden(void)
{
	abort();
}

static int sim_execve(const char *p, char *const *a, char *const *e)
{
	(void)p;
	(void)a;
	(void)e;
	sim_forbidden();
}

static int sim_reboot(int how)
{
	(void)how;
	sim_forbidden();
}

static void sim_sync(void)
{
	sim_forbidden();
}

static int sim_mount(const char *a, const char *b, const char *c, unsigned long d, const void *e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	sim_forbidden();
}

static int sim_umount2(const char *a, int b)
{
	(void)a;
	(void)b;
	sim_forbidden();
}

static int sim_swapoff(const char *a)
{
	(void)a;
	sim_forbidden();
}

#define clock_gettime sim_clock_gettime
#define pipe2 sim_pipe2
#define fork sim_fork
#define kill sim_kill
#define waitpid sim_waitpid
#define open sim_open
#define execve sim_execve
#define reboot sim_reboot
#define sync sim_sync
#define mount sim_mount
#define umount2 sim_umount2
#define swapoff sim_swapoff
#define main ninit_main
int ninit_main(int argc, char **argv);
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
#include "../../src/teardown.c"
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

static void test_put(void)
{
	char b[64];
	size_t n;

	n = put_num(b, 0, 0);
	is_int(n, 1, "put_num 0 is one digit");
	b[n] = '\0';
	is_str(b, "0", "put_num 0");
	n = put_num(b, 0, 4294967295u);
	b[n] = '\0';
	is_str(b, "4294967295", "put_num of the largest unsigned");
	n = put_str(b, 0, "errno ");
	n = put_num(b, n, 13);
	b[n] = '\0';
	is_str(b, "errno 13", "put_str then put_num");
}

static void test_unescape(void)
{
	static const struct {
		const char *in, *out;
	} v[] = {
		{ "/mnt/a\\040b", "/mnt/a b" },
		{ "/x\\011y", "/x\ty" },
		{ "/back\\134slash", "/back\\slash" },
		{ "/nl\\012", "/nl\n" },
		{ "/two\\040\\040", "/two  " },
		{ "/bad\\8", "/bad\\8" },
		{ "/short\\04", "/short\\04" },
		{ "/end\\", "/end\\" },
		{ "/plain", "/plain" },
		{ "", "" },
	};

	for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		char buf[64], what[96];

		snprintf(buf, sizeof(buf), "%s", v[k].in);
		unescape_mount(buf);
		snprintf(what, sizeof(what), "unescape_mount case %zu", k);
		is_str(buf, v[k].out, what);
	}
}

static void test_rtc_local(void)
{
	static const struct {
		const char *text;
		int local;
	} v[] = {
		{ "0.0 0 0.0\n0\nLOCAL\n", 1 },
		{ "0.0 0 0.0\n0\nLOCAL", 1 },
		{ "0.0 0 0.0\n0\nUTC\n", 0 },
		{ "0.0 0 0.0\n0\nLOCALTIME\n", 0 },
		{ "0.0 0 0.0\n0\nlocal\n", 0 },
		{ "0.0 0 0.0\nLOCAL\n", 0 },
		{ "LOCAL\n", 0 },
		{ "", 0 },
	};
	char path[] = "/tmp/ninit-t-ninit-adjtime.XXXXXX";
	int fd = mkstemp(path);

	close(fd);
	adjtime_path = path;
	for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		fd = open(path, O_WRONLY | O_TRUNC);
		(void)!write(fd, v[k].text, strlen(v[k].text));
		close(fd);
		ok(rtc_local() == v[k].local, "adjtime case %zu is %s", k, v[k].local ? "LOCAL" : "UTC");
	}
	unlink(path);
	adjtime_path = NULL;
	ok(rtc_local() == 0, "without /etc/adjtime the clock is UTC");
}

// random insertions and deletions against a plain array model
static void test_pid_table(void)
{
	pid_t model[64];
	uint32_t mval[64];
	int nm = 0, bad = 0, it;

	pid_mask = 63;
	pid_key = calloc(64, sizeof(*pid_key));
	pid_val = calloc(64, sizeof(*pid_val));
	runs = calloc(64, sizeof(*runs));
	n_live = 0;

	for (it = 0; it < 200000 && !bad; it++) {
		if (nm < 40 && (nm == 0 || tap_below(3))) {
			// pids that collide in the low bits stress the probing
			pid_t p = (pid_t)(1 + tap_below(4) * 64 + tap_below(8) * 4096 + tap_below(200));
			int dup = 0;

			for (int k = 0; k < nm; k++)
				dup |= model[k] == p;
			if (dup)
				continue;
			model[nm] = p;
			mval[nm] = tap_below(64);
			runs[mval[nm]].pid = p;
			pid_put(p, mval[nm]);
			nm++;
		} else {
			int k = (int)tap_below((uint32_t)nm);

			pid_del(model[k]);
			model[k] = model[--nm];
			mval[k] = mval[nm];
		}
		for (int k = 0; k < nm; k++) {
			int stale;
			uint32_t got;

			runs[mval[k]].pid = model[k];
			got = find_pid(model[k], &stale);
			if (got != mval[k]) {
				tap_diag("iteration %d: pid %d maps to %u, want %u", it, (int)model[k], got,
					 mval[k]);
				bad = 1;
				break;
			}
		}
		{
			int stale;
			pid_t absent = (pid_t)(900000 + tap_below(1000));

			if (find_pid(absent, &stale) != UINT32_MAX) {
				tap_diag("iteration %d: absent pid %d found", it, (int)absent);
				bad = 1;
			}
		}
	}
	ok(!bad, "the pid table agrees with a model over 200000 random operations");
	{
		uint32_t used = 0;

		for (uint32_t k = 0; k <= pid_mask; k++)
			used += pid_key[k] != 0;
		is_int(used, nm, "the table holds exactly the live entries");
	}
	free(pid_key);
	free(pid_val);
	free(runs);
	pid_key = NULL;
	pid_val = NULL;
	runs = NULL;
}

// state is global in ninit.c, everything below drives the real functions

static struct gb_img sim_img;

static void sim_reset(void)
{
	for (int k = 0; k < n_kids; k++)
		kid_close(&kids[k]);
	n_kids = n_exits = n_pend_w = 0;
	while (n_ctl)
		ctl_drop(&ctl_conn[n_ctl - 1]);
	for (uint32_t i = 0; runs && i < n_svc; i++) {
		if (runs[i].out_fd >= 0)
			close(runs[i].out_fd);
		if (runs[i].ntf_fd >= 0)
			close(runs[i].ntf_fd);
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

static void sim_load(const struct gb_svc *sv, uint32_t n, const struct gb_edge *e, uint32_t m)
{
	sim_reset();
	sim_img = gb_build(sv, n, e, m);
	if (ng_verify(sim_img.map, sim_img.len))
		abort();
	map = sim_img.map;
	start_graph();
}

static struct sim_child *kid_svc(uint32_t i)
{
	return runs[i].pid > 0 ? kid_of(runs[i].pid) : NULL;
}

static void sim_drain(void)
{
	drain_left = DRAIN_BUDGET;
	drain_all();
}

// a daemon without notify is ready once its shell execs, which closes the exec pipe
static void sim_exec_all(void)
{
	for (uint32_t i = 0; i < n_svc; i++) {
		struct sim_child *c;

		if (!runs[i].ntf_exec || !runs[i].starting || runs[i].pid <= 0)
			continue;
		c = kid_of(runs[i].pid);
		if (c && c->alive && c->ntf_w >= 0) {
			close(c->ntf_w);
			c->ntf_w = -1;
		}
	}
}

static void sim_step(void)
{
	for (int round = 0; round < 4; round++) {
		sim_exec_all();
		log_batch_begin();
		fire_restarts();
		check_timeouts();
		ctl_tick();
		for (int c = n_ctl; c-- > 0;)
			if (ctl_conn[c].out_at < ctl_conn[c].out_len || ctl_conn[c].listing ||
			    ctl_conn[c].done)
				ctl_pump(&ctl_conn[c]);
		check_boot_done();
		sim_drain();
		reap();
		sim_drain();
	}
}

static void sim_advance(long long ms)
{
	while (ms > 0) {
		long long d = ms > 50 ? 50 : ms;

		fake_ms += d;
		ms -= d;
		sim_step();
	}
}

// the service became ready the way its type makes it ready
static void sim_ready(uint32_t i)
{
	struct sim_child *c = kid_svc(i);

	if (!c)
		return;
	if (ng_svcs(map)[i].type == NG_TYPE_DAEMON) {
		if (ng_notify(map, i)) {
			(void)!write(c->ntf_w, "\n", 1);
		} else if (c->ntf_w >= 0) {
			close(c->ntf_w);
			c->ntf_w = -1;
		}
	} else {
		kid_die(c, 0);
	}
	sim_step();
}

static void sim_exit_svc(uint32_t i, int status)
{
	struct sim_child *c = kid_svc(i);

	if (c)
		kid_die(c, status);
	sim_step();
}

static void sim_say(uint32_t i, const char *text)
{
	struct sim_child *c = kid_svc(i);

	if (c && c->out_w >= 0)
		(void)!write(c->out_w, text, strlen(text));
	sim_step();
}

static uint32_t svc_by(const char *name)
{
	return ctl_find(name);
}

// sends REQ as a control client, returns the reply once ninit has finished it
static const char *sim_ctl(const char *req)
{
	static char reply[CTL_OUT * 4];
	int sv[2];
	struct ctl *c;
	char line[CTL_BUF];
	size_t at = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) < 0 || n_ctl == CTL_MAX)
		abort();
	c = &ctl_conn[n_ctl++];
	memset(c, 0, sizeof(*c));
	c->fd = sv[0];
	c->svc = UINT32_MAX;
	snprintf(line, sizeof(line), "%s", req);
	ctl_cmd(c, line);
	ctl_pump(c);
	for (int k = 0; k < 400; k++) {
		ssize_t n = read(sv[1], reply + at, sizeof(reply) - 1 - at);

		if (n > 0) {
			at += (size_t)n;
			reply[at] = '\0';
			if (strstr(reply, "\n+ ") || strstr(reply, "\n- ") || !strncmp(reply, "+ ", 2) ||
			    !strncmp(reply, "- ", 2))
				break;
			continue;
		}
		if (n == 0)
			break;
		sim_advance(20);
	}
	reply[at] = '\0';
	close(sv[1]);
	sim_step();
	return reply;
}

static const char *st_of(const char *name)
{
	return state_name(svc_by(name));
}

// counters ninit keeps alongside the state array must match it
static const char *invariants(void)
{
	uint32_t a = 0, d = 0, p = 0, u = 0;

	for (uint32_t i = 0; i < n_svc; i++) {
		a += state[i] == NG_ST_RUNNING;
		d += state[i] == NG_ST_DONE;
		p += state[i] == NG_ST_PENDING;
		u += up[i] != 0;
		// a completed oneshot run again by start stays up for its dependents while it runs
		if (up[i] && state[i] != NG_ST_DONE && state[i] != NG_ST_RUNNING)
			return "a service is up without being done";
		if (runs[i].pid > 0) {
			struct sim_child *c = kid_of(runs[i].pid);
			int stale;

			if (!c)
				return "a service names a pid that was never forked";
			if (find_pid(runs[i].pid, &stale) != i)
				return "a running pid is missing from the pid table";
		}
	}
	for (uint32_t k = 0; k < n_live; k++) {
		if (runs[live[k]].live_pos != k)
			return "the live list and its positions disagree";
		for (uint32_t j = k + 1; j < n_live; j++)
			if (live[j] == live[k])
				return "a service is in the live list twice";
	}
	if (a != n_active)
		return "n_active does not count the starting services";
	if (d != n_done)
		return "n_done does not count the done services";
	if (p != n_pending)
		return "n_pending does not count the pending services";
	if (u != n_up)
		return "n_up does not count the up services";
	return NULL;
}

static int inv_ok(const char *what)
{
	const char *why = invariants();

	if (why) {
		ok(0, "%s: %s", what, why);
		return 0;
	}
	return 1;
}

static void test_sim_basic(void)
{
	static const struct gb_svc sv[] = {
		GB_ONESHOT("a", "x"),
		{ .name = "d", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .notify = 3, .script = "x" },
		GB_ONESHOT("b", "x"),
		GB_TARGET("t"),
		GB_ONESHOT("late", "x"),
	};
	static const struct gb_edge e[] = { { 0, 2 }, { 1, 3 }, { 2, 3 }, { 3, 4 } };

	sim_load(sv, 5, e, 4);
	ok(inv_ok("after start_graph"), "boot starts with consistent counters");
	is_str(st_of("a"), "starting", "a root oneshot is starting");
	is_str(st_of("d"), "starting", "a root daemon is starting");
	is_str(st_of("b"), "pending", "a dependent is pending");
	sim_ready(svc_by("a"));
	is_str(st_of("a"), "up", "a oneshot that exits 0 is up");
	is_str(st_of("b"), "starting", "its dependent starts at once");
	sim_ready(svc_by("b"));
	is_str(st_of("t"), "pending", "a target waits for every prerequisite");
	sim_say(svc_by("d"), "part");
	is_str(st_of("d"), "starting", "notify bytes without a newline are not readiness");
	sim_ready(svc_by("d"));
	is_str(st_of("t"), "up", "a target is up once its prerequisites are");
	is_str(st_of("late"), "starting", "and releases its dependents");
	sim_ready(svc_by("late"));
	sim_advance(10);
	ok(boot_ms >= 0, "the boot summary is printed once nothing is starting");
	ok(inv_ok("after boot"), "the counters agree after boot");
	is_int(n_up, 5, "all five services are up");
}

static void test_sim_restart_backoff(void)
{
	static const struct gb_svc sv[] = {
		{ .name = "r", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .restart = 1, .notify = 3,
		  .script = "x", .pol = { .start_ms = 300 } },
	};
	long long expect[] = { 100, 250, 500, 1000, 2000, 5000, 5000 };
	uint32_t i;
	int bad = 0;

	sim_load(sv, 1, NULL, 0);
	i = svc_by("r");
	sim_ready(i);
	for (size_t k = 0; k < sizeof(expect) / sizeof(*expect); k++) {
		long long t0;

		sim_exit_svc(i, 1 << 8);
		t0 = fake_ms;
		if (runs[i].restart_at - t0 != expect[k] && bad++ < 3)
			tap_diag("exit %zu: restart in %lld ms, want %lld", k, runs[i].restart_at - t0, expect[k]);
		sim_advance(expect[k] + 50);
		sim_ready(i);
	}
	ok(!bad, "restart delays run 100, 250, 500, 1000, 2000, 5000, 5000 ms");
	sim_advance(10000);
	sim_exit_svc(i, 1 << 8);
	is_int(runs[i].restart_at - fake_ms, 100, "10 s up resets the delay to 100 ms");
	ok(inv_ok("restart"), "the counters agree through restarts");

	// a respawn that never becomes ready is killed at its start-timeout, then waits 250 ms
	sim_advance(150);
	is_str(st_of("r"), "down", "a respawning daemon is down until ready");
	{
		int forks = n_kids;
		long long t_kill = 0, t_spawn = 0;

		for (int k = 0; k < 200 && !t_spawn; k++) {
			fake_ms += 5;
			sim_step();
			if (!t_kill && runs[i].timedout)
				t_kill = fake_ms;
			if (t_kill && n_kids > forks + 0 && runs[i].pid > 0 && !runs[i].timedout)
				t_spawn = fake_ms;
		}
		ok(t_kill && t_spawn && t_spawn - t_kill >= 250,
		   "a respawn killed at its timeout waits its 250 ms backoff (respawned after %lld ms)",
		   t_spawn - t_kill);
	}
	ok(inv_ok("timeout respawn"), "the counters agree after timed out respawns");
}

static void test_sim_ctl(void)
{
	static const struct gb_svc sv[] = {
		{ .name = "d", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .script = "x",
		  .pol = { .stop_ms = 200 } },
		GB_ONESHOT("o", "x"),
		GB_TARGET("t"),
	};
	static const struct gb_edge e[] = { { 0, 2 } };
	const char *r;
	uint32_t d;

	sim_load(sv, 3, e, 1);
	d = svc_by("d");
	sim_ready(d);
	sim_ready(svc_by("o"));
	r = sim_ctl("status");
	has_str(r, ". d up wanted pid ", "status lists d");
	has_str(r, "+ 3 services, 3 up\n", "status ends with the count");
	r = sim_ctl("stop d");
	is_str(r, "+ d stopped\n", "stop of a daemon that exits on SIGTERM");
	is_str(st_of("d"), "down", "it is down");
	is_str(st_of("t"), "down", "and so is the target over it");
	r = sim_ctl("stop d");
	is_str(r, "+ d already stopped\n", "stopping it again");
	r = sim_ctl("start d");
	is_str(r, "+ d up\n", "start brings it back once its shell execs");
	{
		struct sim_child *c = kid_svc(d);

		if (c)
			c->ignore_term = 1;
		ok(c != NULL, "the restarted daemon has a process");
	}
	r = sim_ctl("stop d");
	is_str(r, "+ d stopped after SIGKILL\n", "a daemon ignoring SIGTERM is killed after stop-timeout");
	r = sim_ctl("start t");
	is_str(r, "- t is a target, it has no process\n", "a target cannot be started");
	r = sim_ctl("start zz");
	is_str(r, "- no service named zz\n", "an unknown name is refused");
	r = sim_ctl("frob");
	is_str(r, "- unknown command 'frob'\n", "an unknown verb is refused");
	r = sim_ctl("resume");
	is_str(r, "+ nothing to resume\n", "resume with nothing failed");
	ok(inv_ok("ctl"), "the counters agree after control operations");
}

// a refused start must not leave a service counted up while it is listed pending
static void test_sim_refused(void)
{
	static const struct gb_svc sv[] = {
		{ .name = "p", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .script = "x" },
		GB_ONESHOT("o", "x"),
		GB_TARGET("t"),
	};
	static const struct gb_edge e[] = { { 0, 1 }, { 1, 2 } };
	const char *r;

	sim_load(sv, 3, e, 2);
	sim_step();
	sim_ready(svc_by("o"));
	is_str(st_of("o"), "up", "o is up");
	is_str(st_of("t"), "up", "t is up");
	sim_ctl("stop p");
	r = sim_ctl("start o");
	is_str(r, "- o waiting on a prerequisite\n", "start of o is refused while p is stopped");
	is_str(st_of("o"), "pending", "o is left pending");
	r = sim_ctl("status");
	{
		uint32_t rows = 0;
		const char *p = r;
		char expect[64];

		while ((p = strstr(p, " up ")) != NULL) {
			rows++;
			p++;
		}
		snprintf(expect, sizeof(expect), "+ 3 services, %u up\n", rows);
		has_str(r, expect, "the up count matches the services listed up");
	}
	is_str(st_of("t"), "down", "a target over a pending prerequisite is down");
}

static void random_graph(struct gb_svc *sv, uint32_t *n, struct gb_edge *e, uint32_t *m)
{
	static char names[16][8];
	uint32_t roots, k;

	*n = 3 + tap_below(10);
	*m = 0;
	roots = 1 + tap_below(*n / 2 + 1);
	for (k = 0; k < *n; k++) {
		snprintf(names[k], sizeof(names[k]), "s%u", k);
		sv[k] = (struct gb_svc){ .name = names[k], .onfail = GB_DEFAULT, .script = "x" };
		switch (tap_below(4)) {
		case 0:
			sv[k].type = NG_TYPE_TARGET;
			break;
		case 1:
		case 2:
			sv[k].type = NG_TYPE_DAEMON;
			sv[k].restart = (uint8_t)tap_below(2);
			sv[k].notify = tap_below(2) ? 3 : 0;
			break;
		default:
			sv[k].type = NG_TYPE_ONESHOT;
		}
		sv[k].pol.start_ms = 100 + tap_below(3) * 500;
		sv[k].pol.stop_ms = 100 + tap_below(2) * 300;
		sv[k].pol.start_tries = (uint8_t)(1 + tap_below(3));
		sv[k].pol.retry_ms = (uint16_t)tap_below(200);
		sv[k].pol.pflags = tap_below(4) ? 0 : NG_PF_ORDER_ONLY;
		if (k < roots)
			continue;
		// a non-root needs a prerequisite before it
		e[(*m)++] = (struct gb_edge){ tap_below(k), k };
		if (k > 1 && tap_below(2)) {
			uint32_t a = tap_below(k);

			if (a != e[*m - 1].a)
				e[(*m)++] = (struct gb_edge){ a, k };
		}
	}
	for (k = 0; k < *n; k++) {
		int has_dep = 0;

		for (uint32_t j = 0; j < *m; j++)
			has_dep |= e[j].a == k;
		if (!has_dep && !tap_below(4))
			sv[k].onfail = NG_ONFAIL_WARN;
		else if (tap_below(6) == 0)
			sv[k].onfail = NG_ONFAIL_SHELL;
	}
}

static int sim_trace;

static void trace_states(void)
{
	if (!sim_trace)
		return;
	fputs("#     ", stdout);
	for (uint32_t i = 0; i < n_svc; i++)
		printf(" %s=%s%s%s", ng_name(map, i), state_name(i), up[i] ? "^" : "", want[i] ? "!" : "");
	putchar('\n');
}

static void test_sim_fuzz(void)
{
	static const char *const verbs[] = { "start", "stop", "restart", "status", "resume" };
	struct gb_svc sv[16];
	struct gb_edge e[32];
	uint32_t n, m;
	int bad = 0, graphs = 3000, steps = 400;

	const char *only = getenv("NINIT_SIM_SEED");

	if (only) {
		tap_rng_state = strtoull(only, NULL, 0);
		graphs = 1;
		sim_trace = 1;
	}
	for (int g = 0; g < graphs && !bad; g++) {
		uint64_t seed = tap_rng_state;

		random_graph(sv, &n, e, &m);
		sim_load(sv, n, e, m);
		if (sim_trace)
			for (uint32_t k = 0; k < n; k++)
				printf("# svc %s type %s restart %d notify %d onfail %d tries %d order %d\n",
				       sv[k].name, ng_typename(sv[k].type), sv[k].restart, sv[k].notify,
				       ng_onfail(map, k), sv[k].pol.start_tries, sv[k].pol.pflags);
		for (int s = 0; s < steps && !bad; s++) {
			uint32_t i = tap_below(n);
			uint32_t act = tap_below(8);
			const char *why;

			if (sim_trace)
				printf("# step %d action %u on s%u\n", s, act, i);
			switch (act) {
			case 0:
			case 1:
				sim_ready(i);
				break;
			case 2:
				sim_exit_svc(i, tap_below(2) ? 1 << 8 : SIGKILL);
				break;
			case 3:
				sim_say(i, tap_below(2) ? "line\n" : "partial");
				break;
			case 4: {
				char req[32];

				snprintf(req, sizeof(req), "%s s%u", verbs[tap_below(5)], i);
				if (strstr(req, "resume") || strstr(req, "status s"))
					snprintf(req, sizeof(req), "%s", tap_below(2) ? "resume" : "status");
				if (sim_trace)
					printf("#   request %s\n", req);
				sim_ctl(req);
				break;
			}
			case 5: {
				struct sim_child *c = kid_svc(i);

				if (c)
					c->ignore_term = 1;
				break;
			}
			default:
				sim_advance(tap_below(800));
			}
			trace_states();
			why = invariants();
			if (why) {
				tap_diag("graph %d (seed %#llx) step %d: %s", g, (unsigned long long)seed, s, why);
				bad = 1;
			}
		}
	}
	ok(!bad, "%d random graphs, %d random events each, keep the counters consistent", graphs, steps);
}

int main(void)
{
	logcap_quiet();
	test_put();
	test_unescape();
	test_rtc_local();
	test_pid_table();
	test_sim_basic();
	test_sim_restart_backoff();
	test_sim_ctl();
	test_sim_refused();
	test_sim_fuzz();
	sim_reset();
	return tap_done();
}
