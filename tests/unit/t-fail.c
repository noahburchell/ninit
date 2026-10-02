#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "tap.h"

// nothing here may fork, signal or reach the real console

static long long fake_ms = 100000;

static int shim_clock_gettime(clockid_t id, struct timespec *ts)
{
	(void)id;
	ts->tv_sec = fake_ms / 1000;
	ts->tv_nsec = (fake_ms % 1000) * 1000000;
	return 0;
}

static int console_fd = -1, console_errno = ENOENT;
static const char *oom_path, *shadow_path;

static int shim_open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/dev/console")) {
		if (console_fd < 0) {
			errno = console_errno;
			return -1;
		}
		return fcntl(console_fd, F_DUPFD_CLOEXEC, 3);
	}
	if (!strcmp(path, "/proc/self/oom_score_adj")) {
		if (!oom_path) {
			errno = ENOENT;
			return -1;
		}
		path = oom_path;
	}
	return open(path, flags);
}

[[maybe_unused]] static FILE *shim_fopen(const char *path, const char *mode)
{
	if (!strcmp(path, "/etc/shadow")) {
		if (!shadow_path) {
			errno = EACCES;
			return NULL;
		}
		path = shadow_path;
	}
	return fopen(path, mode);
}

// the barrier pipe is the last one created before fork
static int last_pipe_w = -1, child_bar = -1, fork_fail;
static pid_t next_pid = 4000000;

static int shim_pipe2(int p[2], int flags)
{
	int r = pipe2(p, flags);

	if (!r)
		last_pipe_w = p[1];
	return r;
}

static pid_t shim_fork(void)
{
	if (fork_fail) {
		errno = EAGAIN;
		return -1;
	}
	if (child_bar >= 0)
		close(child_bar);
	child_bar = fcntl(last_pipe_w, F_DUPFD_CLOEXEC, 3);
	return ++next_pid;
}

static int fake_vt, vt_mode, kb_mode, set_mode = -1, set_kb = -1;

// defined after fail.c, which owns the KD request numbers
static int shim_ioctl(int fd, unsigned long req, ...);

static int force_nosys;

static long shim_syscall(long nr, ...)
{
	va_list ap;
	unsigned a, b, c;

	va_start(ap, nr);
	a = va_arg(ap, unsigned);
	b = va_arg(ap, unsigned);
	c = va_arg(ap, unsigned);
	va_end(ap);
	if (force_nosys) {
		errno = ENOSYS;
		return -1;
	}
	return syscall(nr, a, b, c);
}

static int shim_execve(const char *p, char *const *a, char *const *e)
{
	(void)p;
	(void)a;
	(void)e;
	abort();
}

#define clock_gettime shim_clock_gettime
#define open shim_open
#define fopen shim_fopen
#define pipe2 shim_pipe2
#define fork shim_fork
#define ioctl shim_ioctl
#define syscall shim_syscall
#define execve shim_execve
#include "../../src/fail.c"
#undef clock_gettime
#undef open
#undef fopen
#undef pipe2
#undef fork
#undef ioctl
#undef syscall
#undef execve

#include "gbuild.h"
#include "logcap.h"

static int shim_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (fake_vt) {
		switch (req) {
		case NG_KDGKBTYPE:
			*(char *)arg = 2;
			return 0;
		case NG_KDGETMODE:
			*(int *)arg = vt_mode;
			return 0;
		case NG_KDSETMODE:
			set_mode = (int)(intptr_t)arg;
			return 0;
		case NG_KDGKBMODE:
			*(int *)arg = kb_mode;
			return 0;
		case NG_KDSKBMODE:
			set_kb = (int)(intptr_t)arg;
			return 0;
		default:
			break;
		}
	}
	return ioctl(fd, req, arg);
}


static void test_describe(void)
{
	static const struct {
		int st;
		const char *want;
	} v[] = {
		{ 0, "exit 0" },
		{ 1 << 8, "exit 1" },
		{ 255 << 8, "exit 255" },
		{ 127 << 8, "exit 127" },
		{ SIGKILL, "killed by SIGKILL" },
		{ SIGTERM, "killed by SIGTERM" },
		{ SIGHUP, "killed by SIGHUP" },
		{ SIGSEGV | 0x80, "killed by SIGSEGV (core dumped)" },
		{ SIGABRT | 0x80, "killed by SIGABRT (core dumped)" },
		{ SIGSYS, "killed by SIGSYS" },
		{ SIGPWR, "killed by SIGPWR" },
		{ 40, "killed by signal 40" },
		{ 64 | 0x80, "killed by signal 64 (core dumped)" },
		{ 0x137f, "status 0x137f" },
		{ 0xffff, "status 0xffff" },
		{ FAIL_ST_NOTIFY_HUP, "closed its notify fd before reporting ready" },
		{ FAIL_ST_TIMEOUT, "timed out and was killed" },
	};
	char buf[64];

	for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		char what[64];

		fail_describe(v[k].st, buf, sizeof(buf));
		snprintf(what, sizeof(what), "status %#x reads as %s", (unsigned)v[k].st, v[k].want);
		is_str(buf, v[k].want, what);
	}
	// volatile keeps lto from flagging the intended truncation
	{
		volatile size_t cap = 8;

		fail_describe(SIGKILL, buf, cap);
		is_str(buf, "killed ", "a short buffer is cut and terminated");
	}
}

// a(0) x(1) | b(2) <- a, t(3) target <- b, d(4) <- t, e(5) <- a x
static struct gb_img poison_graph(void)
{
	static const struct gb_svc sv[] = {
		GB_ONESHOT("a", ":"), GB_ONESHOT("x", ":"), GB_ONESHOT("b", ":"),
		GB_TARGET("t"), GB_ONESHOT("d", ":"), GB_ONESHOT("e", ":"),
	};
	static const struct gb_edge e[] = { { 0, 2 }, { 2, 3 }, { 3, 4 }, { 0, 5 }, { 1, 5 } };

	return gb_build(sv, 6, e, 5);
}

static void test_poison(void)
{
	struct gb_img g = poison_graph();
	uint8_t st[6], up[6];
	uint32_t n, undone;

	is_str(ng_verify(g.map, g.len), NULL, "poison graph verifies");

	memset(st, NG_ST_PENDING, sizeof(st));
	memset(up, 0, sizeof(up));
	st[0] = NG_ST_RUNNING;
	n = fail_poison(g.map, 0, st, up, &undone);
	is_int(n, 4, "a failed root skips its four pending dependents");
	is_int(undone, 0, "none of them had completed");
	is_int(st[0], NG_ST_FAILED, "the service itself is failed");
	ok(st[2] == NG_ST_SKIPPED && st[3] == NG_ST_SKIPPED && st[4] == NG_ST_SKIPPED &&
	   st[5] == NG_ST_SKIPPED, "every dependent is skipped, transitively");
	is_int(st[1], NG_ST_PENDING, "an unrelated root is untouched");

	memset(st, NG_ST_PENDING, sizeof(st));
	memset(up, 0, sizeof(up));
	st[5] = NG_ST_RUNNING;
	st[3] = NG_ST_DONE;
	n = fail_poison(g.map, 0, st, up, &undone);
	is_int(n, 2, "pending b and d are skipped");
	is_int(undone, 1, "a completed target that is down is undone");
	is_int(st[3], NG_ST_SKIPPED, "the down target is skipped");
	is_int(st[5], NG_ST_RUNNING, "a running dependent is never stopped");

	memset(st, NG_ST_PENDING, sizeof(st));
	memset(up, 0, sizeof(up));
	st[2] = NG_ST_DONE;
	up[2] = 1;
	st[3] = NG_ST_DONE;
	up[3] = 1;
	n = fail_poison(g.map, 0, st, up, &undone);
	is_int(n, 1, "only e is pending behind a");
	is_int(undone, 0, "an up target is not undone");
	is_int(st[3], NG_ST_DONE, "an up target stays done");
	is_int(st[4], NG_ST_PENDING, "and what is behind it is not reached");

	memset(st, NG_ST_PENDING, sizeof(st));
	memset(up, 0, sizeof(up));
	st[0] = NG_ST_DONE;
	n = fail_poison_deps(g.map, 0, st, up, &undone);
	is_int(n, 4, "a completed service that exits without restart skips its pending dependents");
	is_int(st[0], NG_ST_DONE, "it stays done itself");

	memset(st, NG_ST_DONE, sizeof(st));
	memset(up, 1, sizeof(up));
	n = fail_poison_deps(g.map, 0, st, up, &undone);
	is_int(n, 0, "nothing is skipped when every dependent has started");
	is_int(undone, 0, "and nothing is undone");

	memset(st, NG_ST_PENDING, sizeof(st));
	memset(up, 0, sizeof(up));
	n = fail_poison_deps(g.map, 4, st, up, &undone);
	is_int(n, 0, "a service without dependents skips nothing");

	// a skipped service already counted is not counted again
	memset(st, NG_ST_PENDING, sizeof(st));
	memset(up, 0, sizeof(up));
	st[1] = NG_ST_FAILED;
	st[5] = NG_ST_SKIPPED;
	n = fail_poison(g.map, 0, st, up, &undone);
	is_int(n, 3, "a dependent skipped before is not counted again");
	free(g.map);
}

static struct gb_img fail_graph(void)
{
	static const struct gb_svc sv[] = {
		GB_ONESHOT("root", ":"),
		{ .name = "sh", .type = NG_TYPE_ONESHOT, .onfail = NG_ONFAIL_SHELL, .script = ":" },
		GB_ONESHOT("leaf", ":"),
		{ .name = "d", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .restart = 1, .script = ":" },
		GB_ONESHOT("one", ":"),
		GB_ONESHOT("two", ":"),
		GB_ONESHOT("three", ":"),
	};
	static const struct gb_edge e[] = { { 0, 4 }, { 0, 5 }, { 1, 6 }, { 4, 6 }, { 5, 6 } };

	return gb_build(sv, 7, e, 5);
}

static void test_fail_service(void)
{
	struct gb_img g = fail_graph();
	const char *t;
	enum fail_act a;

	is_str(ng_verify(g.map, g.len), NULL, "fail graph verifies");

	logcap_begin();
	a = fail_service(g.map, 0, 1 << 8, 1, 3, "", 0);
	is_int(a, FAIL_RETRY, "attempts left means retry");
	has_str(logcap_text(), "WARN > root: failed (exit 1), retrying (1 of 3)\n", "retry is reported");

	logcap_begin();
	a = fail_service(g.map, 0, 1 << 8, 3, 3, "out one\nout two\n", 16);
	t = logcap_text();
	is_int(a, FAIL_STOP, "a service with dependents stops them by default");
	has_str(t, "FAIL > root: failed 3 times (exit 1)\n", "the final failure is reported");
	has_str(t, "FAIL > out one\n", "the output tail is logged");
	has_str(t, "FAIL > out two\n", "line by line");
	has_str(t, "FAIL > root: 3 services depend on it, stopping\n", "the stop is reported");
	ok(strstr(t, "failed 3 times") < strstr(t, "out one") &&
	   strstr(t, "out two") < strstr(t, "stopping"), "the tail sits between the two");

	logcap_begin();
	fail_service(g.map, 4, SIGKILL, 1, 1, NULL, 0);
	t = logcap_text();
	has_str(t, "FAIL > one: failed 1 time (killed by SIGKILL)\n", "one attempt is singular");
	has_str(t, "FAIL > one: 1 service depends on it, stopping\n", "one dependent is singular");

	logcap_begin();
	a = fail_service(g.map, 1, FAIL_ST_TIMEOUT, 1, 1, NULL, 0);
	t = logcap_text();
	is_int(a, FAIL_SHELL, "onfail shell asks for the shell");
	has_str(t, "FAIL > sh: failed 1 time (timed out and was killed)\n", "the timeout is reported");
	has_str(t, "FAIL > sh: 1 of 7 services depends on it, starting emergency shell\n",
		"the shell is announced with the share of the graph");

	logcap_begin();
	a = fail_service(g.map, 2, 2 << 8, 1, 1, NULL, 0);
	t = logcap_text();
	is_int(a, FAIL_WARN, "a service without dependents only warns");
	has_str(t, "WARN > leaf: no dependent services, continuing\n", "the warning is reported");

	logcap_begin();
	fail_service(g.map, 3, FAIL_ST_NOTIFY_HUP, 2, 2, NULL, 0);
	t = logcap_text();
	has_str(t, "FAIL > d: failed 2 times (closed its notify fd before reporting ready)\n",
		"a notify hangup is reported");
	has_str(t, "WARN > d: restart applies after readiness, start-tries is 2\n",
		"a restarting daemon is told restart does not cover its start");
	free(g.map);
}

static void test_summary(void)
{
	struct gb_img g = fail_graph();
	uint8_t st[7];
	const char *t;

	memset(st, NG_ST_DONE, sizeof(st));
	logcap_begin();
	fail_summary(g.map, st);
	is_str(logcap_text(), "", "a clean boot has no failure summary");

	st[0] = NG_ST_FAILED;
	st[4] = st[5] = NG_ST_SKIPPED;
	logcap_begin();
	fail_summary(g.map, st);
	t = logcap_text();
	has_str(t, "FAIL > boot: 1 service failed, 2 never started\n", "the summary counts");
	has_str(t, "FAIL > boot: root failed\n", "each failure is named");
	has_str(t, "WARN > boot: skipped one\n", "each skip is named");
	has_str(t, "WARN > boot: skipped two\n", "every one of them");

	memset(st, NG_ST_DONE, sizeof(st));
	st[6] = NG_ST_SKIPPED;
	logcap_begin();
	fail_summary(g.map, st);
	has_str(logcap_text(), "WARN > boot: 0 services failed, 1 never started\n",
		"skips alone are a warning");
	free(g.map);
}

static void test_cloexec(void)
{
	static const int fds[] = { 3, 4, 5, 9, 63, 200 };
	int tmp = open("/dev/null", O_RDONLY | O_CLOEXEC), null = fcntl(tmp, F_DUPFD_CLOEXEC, 300), bad;
	char what[64];

	close(tmp);
	for (size_t k = 0; k < sizeof(fds) / sizeof(*fds); k++)
		if (fcntl(fds[k], F_GETFD) >= 0) {
			ok(1, "# SKIP fd %d is already open", fds[k]);
			close(null);
			return;
		}

	for (int pass = 0; pass < 2; pass++) {
		force_nosys = pass;
		for (int keep = -1; keep <= 9; keep += (keep < 5 ? 1 : 4)) {
			for (size_t k = 0; k < sizeof(fds) / sizeof(*fds); k++) {
				dup2(null, fds[k]);
				fcntl(fds[k], F_SETFD, 0);
			}
			ninit_cloexec_except(keep);
			bad = 0;
			for (size_t k = 0; k < sizeof(fds) / sizeof(*fds); k++) {
				int fl = fcntl(fds[k], F_GETFD);
				int want = fds[k] == keep ? 0 : FD_CLOEXEC;

				bad += (fl & FD_CLOEXEC) != want;
			}
			bad += fcntl(0, F_GETFD) & FD_CLOEXEC;
			bad += fcntl(2, F_GETFD) & FD_CLOEXEC;
			snprintf(what, sizeof(what), "%s, keep %d", pass ? "fallback loop" : "close_range",
				 keep);
			ok(!bad, "%s: only the kept fd and 0..2 survive exec", what);
		}
	}
	force_nosys = 0;
	for (size_t k = 0; k < sizeof(fds) / sizeof(*fds); k++)
		close(fds[k]);
	close(null);
}

static void test_oom(void)
{
	char path[] = "/tmp/ninit-t-fail-oom.XXXXXX", buf[32];
	int fd = mkstemp(path);
	ssize_t n;

	close(fd);
	oom_path = path;
	ninit_oom_score_adj("-1000\n", 6);
	fd = open(path, O_RDONLY);
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[n > 0 ? n : 0] = '\0';
	is_str(buf, "-1000\n", "oom_score_adj is written");
	oom_path = NULL;
	ninit_oom_score_adj("0\n", 2);
	ok(1, "a missing oom_score_adj is ignored");
	unlink(path);
}

static void emerg_reset(void)
{
	emerg_bar_close();
	if (child_bar >= 0)
		close(child_bar);
	child_bar = -1;
	emerg_pid = -1;
	emerg_fast = emerg_fail = emerg_noexec = 0;
	emerg_gone = emerg_greeted = 0;
	emerg_conf = EMERG_EXEC_UNKNOWN;
	emerg_retry_at = 0;
	fork_fail = 0;
	fake_vt = 0;
}

static void exec_ok(void)
{
	close(child_bar);
	child_bar = -1;
	fail_emergency_report();
}

static void exec_fail(int e0, int e1, size_t len)
{
	int errs[2] = { e0, e1 };

	(void)!write(child_bar, errs, len);
	close(child_bar);
	child_bar = -1;
	fail_emergency_report();
}

static int open_pty(int *master)
{
	int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC), s;

	if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0)
		return -1;
	s = open(ptsname(m), O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (s < 0) {
		close(m);
		return -1;
	}
	*master = m;
	return s;
}

static void test_emergency(void)
{
	int master = -1, slave = open_pty(&master);
	pid_t pid;
	const char *t;

	if (slave < 0) {
		ok(1, "# SKIP no pty for the console");
		return;
	}
	console_fd = slave;

	emerg_reset();
	logcap_begin();
	fail_emergency_shell("boot: cannot continue without fs");
	pid = emerg_pid;
	ok(pid > 0, "the shell is spawned");
	ok(fail_emergency_fd() >= 0, "its exec barrier is watched");
	is_int(fail_emergency_due(), EMERG_EXEC_MS, "the exec status is due within 5 s");
	exec_ok();
	t = logcap_text();
	has_str(t, "FAIL > boot: cannot continue without fs\n", "the reason is logged");
	has_str(t, "DONE > shell: started on the console, exit with reboot or poweroff\n",
		"a shell that execs is announced");
	has_str(t, "NOTE > shell: 'ninitctl resume' retries failed services\n", "with the resume hint");
	is_int(fail_emergency_fd(), -1, "the barrier is closed once settled");
	is_int(fail_emergency_due(), -1, "nothing is due while it runs");

	logcap_begin();
	fail_emergency_shell("again");
	has_str(logcap_text(), "WARN > shell: already running\n", "a second request finds it running");
	is_int(emerg_pid, pid, "and starts nothing");

	is_int(fail_emergency_reaped(pid + 1000, 0), 0, "another pid is not the shell");

	fake_ms += 3000;
	logcap_begin();
	ok(fail_emergency_reaped(pid, 0), "the shell's exit is claimed");
	t = logcap_text();
	has_str(t, "WARN > shell: exited (exit 0)\n", "its exit is reported");
	ok(emerg_pid > 0 && emerg_pid != pid, "it is started again");
	exec_ok();
	is_int(logcap_count("shell: started on the console"), 0, "the greeting is not repeated");

	// five exits within a second of starting end the respawning
	for (int k = 1; k <= 5; k++) {
		pid = emerg_pid;
		fake_ms += 100;
		logcap_begin();
		fail_emergency_reaped(pid, 1 << 8);
		if (k < 5) {
			ok(emerg_pid > 0 && emerg_pid != pid, "fast exit %d of 5 restarts it", k);
			exec_ok();
		}
	}
	t = logcap_text();
	has_str(t, "FAIL > shell: exited immediately 5 times (exit 1), not restarting\n",
		"the fifth fast exit gives up");
	has_str(t, "FAIL > shell: giving up, no shell can be started\n", "permanently");
	is_int(emerg_pid, -1, "nothing is running");
	logcap_begin();
	fail_emergency_shell("later failure");
	has_str(logcap_text(), "FAIL > later failure\n", "a later reason is still logged");
	is_int(emerg_pid, -1, "but no shell is started once given up");

	// a slow exit resets the fast count
	emerg_reset();
	fail_emergency_shell("x");
	exec_ok();
	for (int k = 0; k < 4; k++) {
		fake_ms += 100;
		fail_emergency_reaped(emerg_pid, 0);
		exec_ok();
	}
	fake_ms += 1000;
	fail_emergency_reaped(emerg_pid, 0);
	exec_ok();
	is_int(emerg_fast, 0, "an exit after 1 s resets the count of fast exits");
	ok(!emerg_gone, "and it keeps respawning");

	// two consecutive exec failures end the respawning
	emerg_reset();
	logcap_begin();
	fail_emergency_shell("x");
	pid = emerg_pid;
	exec_fail(ENOENT, ENOENT, sizeof(int[2]));
	t = logcap_text();
	has_str(t, "FAIL > shell: exec /bin/sh: No such file or directory\n", "an exec failure is reported");
	has_str(t, "FAIL > shell: cannot exec a shell: No such file or directory\n", "and settles");
#ifdef NINIT_BUSYBOX
	has_str(t, "WARN > shell: exec " NINIT_BUSYBOX ": No such file or directory\n",
		"the busybox attempt is reported first");
#endif
	logcap_begin();
	fail_emergency_reaped(pid, 127 << 8);
	t = logcap_text();
	ok(!strstr(t, "shell: exited"), "a child that could not exec is not reported as exiting");
	pid = emerg_pid;
	exec_fail(EACCES, EACCES, sizeof(int[2]));
	t = logcap_text();
	has_str(t, "FAIL > shell: giving up, no shell can be started\n", "the second exec failure gives up");
	ok(emerg_gone, "the shell is gone");
	ok(fail_emergency_reaped(pid, 127 << 8), "its exit is still claimed");
	is_int(emerg_pid, -1, "and it is not restarted");

	// an exec failure then a success clears the count
	emerg_reset();
	fail_emergency_shell("x");
	exec_fail(ENOENT, ENOENT, sizeof(int[2]));
	fail_emergency_reaped(emerg_pid, 127 << 8);
	exec_ok();
	is_int(emerg_noexec, 0, "a shell that execs clears the exec failure count");

	emerg_reset();
	logcap_begin();
	fail_emergency_shell("x");
	exec_fail(1, 2, 3);
	has_str(logcap_text(), "WARN > shell: truncated exec status, state unknown\n",
		"a short exec status is reported");

	emerg_reset();
	logcap_begin();
	fail_emergency_shell("x");
	fake_ms += EMERG_EXEC_MS - 1;
	fail_emergency_tick();
	is_int(fail_emergency_due(), 1, "the exec status is due in 1 ms");
	fake_ms += 1;
	fail_emergency_tick();
	has_str(logcap_text(), "WARN > shell: no exec status after 5 s, state unknown\n",
		"no exec status in 5 s is reported");
	is_int(fail_emergency_due(), -1, "and not waited on again");

	// five failures to start, two seconds apart, then nothing more
	emerg_reset();
	console_fd = -1;
	console_errno = EIO;
	logcap_begin();
	fail_emergency_shell("x");
	t = logcap_text();
	{
		char want[128];

		snprintf(want, sizeof(want), "FAIL > shell: no console available: %s\n", strerror(EIO));
		has_str(t, want, "a missing console is reported");
	}
	has_str(t, "FAIL > shell: not running, retrying in 2000 ms\n", "and retried");
	is_int(fail_emergency_due(), EMERG_RETRY_MS, "the retry is due in 2 s");
	for (int k = 2; k <= 5; k++) {
		fake_ms += EMERG_RETRY_MS - 1;
		fail_emergency_tick();
		is_int(emerg_fail, k - 1, "no retry before 2 s have passed");
		fake_ms += 1;
		fail_emergency_tick();
		is_int(emerg_fail, k, "the retry fails again");
	}
	has_str(logcap_text(), "FAIL > shell: giving up after 5 attempts\n", "the fifth failure gives up");
	is_int(fail_emergency_due(), -1, "no retry is due after giving up");
	console_fd = slave;

	emerg_reset();
	fork_fail = 1;
	logcap_begin();
	fail_emergency_shell("x");
	{
		char want[128];

		snprintf(want, sizeof(want), "FAIL > shell: fork: %s\n", strerror(EAGAIN));
		has_str(logcap_text(), want, "a fork failure is reported");
	}
	is_int(emerg_fail, 1, "and counted as a start failure");
	fork_fail = 0;
	fake_ms += EMERG_RETRY_MS;
	fail_emergency_tick();
	ok(emerg_pid > 0, "the retry succeeds once fork does");
	exec_ok();

	// the console is restored to a usable state
	{
		struct termios tt;

		emerg_reset();
		tcgetattr(slave, &tt);
		cfmakeraw(&tt);
		tcsetattr(slave, TCSANOW, &tt);
		logcap_begin();
		fail_emergency_shell("x");
		exec_ok();
		has_str(logcap_text(), "WARN > console: terminal in raw mode, restoring\n",
			"a raw terminal is reported");
		tcgetattr(slave, &tt);
		ok((tt.c_lflag & (ISIG | ICANON | ECHO)) == (ISIG | ICANON | ECHO),
		   "canonical mode, echo and signals are restored");
		ok((tt.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR), "output processing is restored");
		ok(tt.c_cc[VINTR] == 003 && tt.c_cc[VEOF] == 004 && tt.c_cc[VERASE] == 0177,
		   "the control characters are the defaults");
		emerg_reset();
		logcap_begin();
		fail_emergency_shell("x");
		exec_ok();
		ok(!strstr(logcap_text(), "raw mode"), "a sane terminal is left alone");
	}

	emerg_reset();
	fake_vt = 1;
	vt_mode = 1;
	kb_mode = 0;
	set_mode = set_kb = -1;
	logcap_begin();
	fail_emergency_shell("x");
	exec_ok();
	t = logcap_text();
	has_str(t, "WARN > console: in graphics mode, restoring text\n", "a vt in graphics mode is reported");
	is_int(set_mode, NG_KD_TEXT, "and set to text mode");
	has_str(t, "WARN > console: keyboard in raw mode, restoring\n", "a raw keyboard is reported");
	is_int(set_kb, NG_K_UNICODE, "and set to unicode");

	emerg_reset();
	fake_vt = 1;
	vt_mode = NG_KD_TEXT;
	kb_mode = NG_K_XLATE;
	set_mode = set_kb = -1;
	fail_emergency_shell("x");
	exec_ok();
	ok(set_mode < 0 && set_kb < 0, "a vt in text mode with a translating keyboard is left alone");

#ifdef NINIT_SULOGIN
	{
		static const struct {
			const char *shadow, *want;
		} v[] = {
			{ "root:$6$salt$hash:19000:0:::::\n", "the root password is required" },
			{ "bin:*:1::::::\nroot:$y$j9T$x:1::::::\n", "the root password is required" },
			{ "root::19000::::::\n", "root has no usable password, no authentication" },
			{ "root:!:19000::::::\n", "root has no usable password, no authentication" },
			{ "root:!$6$locked:1::::::\n", "root has no usable password, no authentication" },
			{ "root:*:19000::::::\n", "root has no usable password, no authentication" },
			{ "rootx:$6$a$b:1::::::\n", "root has no usable password, no authentication" },
			{ "root:", "root has no usable password, no authentication" },
			{ NULL, "root has no usable password, no authentication" },
		};
		char path[] = "/tmp/ninit-t-fail-shadow.XXXXXX";
		int fd = mkstemp(path);

		close(fd);
		for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
			char what[96], want[128];
			FILE *f;

			if (v[k].shadow) {
				f = fopen(path, "w");
				fputs(v[k].shadow, f);
				fclose(f);
				shadow_path = path;
			} else {
				shadow_path = NULL;
			}
			emerg_reset();
			logcap_begin();
			fail_emergency_shell("x");
			exec_ok();
			snprintf(want, sizeof(want), "WARN > shell: %s\n", v[k].want);
			snprintf(what, sizeof(what), "shadow case %zu: %s", k, v[k].want);
			has_str(logcap_text(), want, what);
		}
		emerg_reset();
		fail_emergency_shell("x");
		exec_ok();
		logcap_begin();
		fail_emergency_reaped(emerg_pid, 0);
		exec_ok();
		ok(!strstr(logcap_text(), "password"), "the password notice is not repeated on respawn");
		unlink(path);
	}
#endif

	emerg_reset();
	console_fd = -1;
	close(slave);
	close(master);
}

int main(void)
{
	int null;

	// runs first, before the low fds are taken
	test_cloexec();
	// the console fallback reads stdin
	null = open("/dev/null", O_RDONLY | O_CLOEXEC);
	dup2(null, 0);
	close(null);
	logcap_quiet();
	test_describe();
	test_poison();
	test_fail_service();
	test_summary();
	test_oom();
	test_emergency();
	return tap_done();
}
