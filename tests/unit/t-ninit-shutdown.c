#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "tap.h"

// every effect of ninit-shutdown lands in this record instead of on the system
static struct {
	int kills, kill_sig, kill_errno;
	pid_t kill_pid;
	int reboots, reboot_cmd, syncs;
	const char *comm;
	const char *self;
	int execs;
	char exec_path[PATH_MAX + 128];
	char exec_argv[8][64];
	int exec_argc;
	int fork_ret, forks;
	int wait_status, wait_eintr;
	char lp_args[12][96];
	int lp_argc;
	unsigned slept, sleep_calls, sleep_short;
	uid_t euid;
	time_t now;
} sh;

static jmp_buf exit_jb;
static char con_path[] = "/tmp/ninit-t-shutdown-console.XXXXXX";

static int shim_kill(pid_t pid, int sig)
{
	sh.kills++;
	sh.kill_pid = pid;
	sh.kill_sig = sig;
	if (sh.kill_errno) {
		errno = sh.kill_errno;
		return -1;
	}
	return 0;
}

static int shim_reboot(int cmd)
{
	sh.reboots++;
	sh.reboot_cmd = cmd;
	errno = EPERM;
	return -1;
}

static void shim_sync(void)
{
	sh.syncs++;
}

static int shim_open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/proc/1/comm")) {
		char tmp[] = "/tmp/ninit-t-shutdown-comm.XXXXXX";
		int fd;

		if (!sh.comm) {
			errno = ENOENT;
			return -1;
		}
		fd = mkstemp(tmp);
		unlink(tmp);
		(void)!write(fd, sh.comm, strlen(sh.comm));
		lseek(fd, 0, SEEK_SET);
		return fd;
	}
	if (!strcmp(path, "/dev/console"))
		return open(con_path, O_WRONLY | O_APPEND | O_CLOEXEC);
	errno = EACCES;
	return -1;
}

static ssize_t shim_readlink(const char *restrict path, char *restrict buf, size_t n)
{
	size_t len;

	if (strcmp(path, "/proc/self/exe") || !sh.self) {
		errno = ENOENT;
		return -1;
	}
	len = strlen(sh.self);
	if (len > n)
		len = n;
	memcpy(buf, sh.self, len);
	return (ssize_t)len;
}

static int shim_execv(const char *path, char *const argv[])
{
	sh.execs++;
	{
		size_t n = strnlen(path, sizeof(sh.exec_path) - 1);

		memcpy(sh.exec_path, path, n);
		sh.exec_path[n] = '\0';
	}
	for (sh.exec_argc = 0; argv[sh.exec_argc] && sh.exec_argc < 8; sh.exec_argc++)
		snprintf(sh.exec_argv[sh.exec_argc], sizeof(sh.exec_argv[0]), "%s", argv[sh.exec_argc]);
	errno = ENOENT;
	return -1;
}

static pid_t shim_fork(void)
{
	sh.forks++;
	if (sh.fork_ret < 0)
		errno = EAGAIN;
	return sh.fork_ret;
}

static int shim_execlp(const char *file, const char *arg, ...)
{
	va_list ap;
	const char *a;

	sh.lp_argc = 0;
	snprintf(sh.lp_args[sh.lp_argc++], sizeof(sh.lp_args[0]), "%s", file);
	va_start(ap, arg);
	for (a = arg; a && sh.lp_argc < 12; a = va_arg(ap, const char *))
		snprintf(sh.lp_args[sh.lp_argc++], sizeof(sh.lp_args[0]), "%s", a);
	va_end(ap);
	errno = ENOENT;
	return -1;
}

static pid_t shim_waitpid(pid_t pid, int *st, int flags)
{
	(void)flags;
	if (sh.wait_eintr) {
		sh.wait_eintr--;
		errno = EINTR;
		return -1;
	}
	*st = sh.wait_status;
	return pid;
}

[[noreturn]] static void shim_exit(int code)
{
	longjmp(exit_jb, 1000 + code);
}

[[noreturn]] static void shim__exit(int code)
{
	longjmp(exit_jb, 2000 + code);
}

static unsigned shim_sleep(unsigned s)
{
	unsigned left = 0;

	sh.sleep_calls++;
	// an interrupted sleep returns the remaining seconds
	if (sh.sleep_short && s > 1) {
		sh.sleep_short--;
		left = s / 2;
	}
	sh.slept += s - left;
	return left;
}

static uid_t shim_geteuid(void)
{
	return sh.euid;
}

static time_t shim_time(time_t *t)
{
	if (t)
		*t = sh.now;
	return sh.now;
}

#define main shutdown_main
#define kill shim_kill
#define reboot shim_reboot
#define sync shim_sync
#define open shim_open
#define readlink shim_readlink
#define execv shim_execv
#define fork shim_fork
#define execlp shim_execlp
#define waitpid shim_waitpid
#define exit shim_exit
#define _exit shim__exit
#define sleep shim_sleep
#define geteuid shim_geteuid
#define time shim_time
int shutdown_main(int argc, char **argv);
#include "../../tools/shutdown.c"
#undef main
#undef kill
#undef reboot
#undef sync
#undef open
#undef readlink
#undef execv
#undef fork
#undef execlp
#undef waitpid
#undef exit
#undef _exit
#undef sleep
#undef geteuid
#undef time

static char out[8192], err[8192], con[1024];

static void slurp(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, buf, cap - 1) : -1;

	buf[n > 0 ? n : 0] = '\0';
	if (fd >= 0)
		close(fd);
}

static void reset(void)
{
	memset(&sh, 0, sizeof(sh));
	sh.comm = "ninit\n";
	sh.self = "/usr/sbin/ninit-shutdown";
	sh.fork_ret = 4242;
	// 12:00:30 on the first day of the epoch, read in UTC
	sh.now = 12 * 3600 + 30;
}

// runs ninit-shutdown with the effects recorded, returns its exit status or 1000 + exit()
static int run(const char *argv0, ...)
{
	static char argbuf[16][128];
	char *argv[17];
	char opath[] = "/tmp/ninit-t-shutdown-out.XXXXXX", epath[] = "/tmp/ninit-t-shutdown-err.XXXXXX";
	int argc = 0, o, e, fo, fe;
	volatile int rc;
	va_list ap;
	const char *a;

	va_start(ap, argv0);
	for (a = argv0; a && argc < 16; a = va_arg(ap, const char *)) {
		snprintf(argbuf[argc], sizeof(argbuf[0]), "%s", a);
		argv[argc] = argbuf[argc];
		argc++;
	}
	va_end(ap);
	argv[argc] = NULL;

	fo = mkstemp(opath);
	fe = mkstemp(epath);
	{
		int c = open(con_path, O_WRONLY | O_TRUNC | O_CLOEXEC);

		close(c);
	}
	fflush(stdout);
	fflush(stderr);
	o = dup(1);
	e = dup(2);
	dup2(fo, 1);
	dup2(fe, 2);
	rc = setjmp(exit_jb);
	if (!rc)
		rc = shutdown_main(argc, argv);
	fflush(stdout);
	fflush(stderr);
	dup2(o, 1);
	dup2(e, 2);
	close(o);
	close(e);
	close(fo);
	close(fe);
	slurp(opath, out, sizeof(out));
	slurp(epath, err, sizeof(err));
	slurp(con_path, con, sizeof(con));
	unlink(opath);
	unlink(epath);
	return rc;
}

static void expect_signal(int rc, int sig, const char *what)
{
	char w[160];

	snprintf(w, sizeof(w), "%s: exits 0", what);
	is_int(rc, 0, w);
	snprintf(w, sizeof(w), "%s: signals pid 1 once with %s", what, strsignal(sig));
	ok(sh.kills == 1 && sh.kill_pid == 1 && sh.kill_sig == sig, "%s", w);
	if (sh.kills != 1 || sh.kill_sig != sig)
		tap_diag("kills %d pid %d sig %d", sh.kills, (int)sh.kill_pid, sh.kill_sig);
}

static void test_names(void)
{
	reset();
	expect_signal(run("poweroff", NULL), SIGUSR2, "poweroff");
	reset();
	expect_signal(run("reboot", NULL), SIGTERM, "reboot");
	reset();
	expect_signal(run("halt", NULL), SIGUSR1, "halt");
	reset();
	expect_signal(run("/usr/sbin/reboot", NULL), SIGTERM, "reboot by full path");
	reset();
	expect_signal(run("shutdown", "now", NULL), SIGUSR2, "shutdown now powers off");
	reset();
	expect_signal(run("ninit-shutdown", "now", NULL), SIGUSR2, "ninit-shutdown now powers off");
	reset();
	expect_signal(run("telinit", "0", NULL), SIGUSR2, "telinit 0");
	reset();
	expect_signal(run("telinit", "6", NULL), SIGTERM, "telinit 6");
	reset();
	expect_signal(run("something-else", NULL), SIGUSR2, "an unknown name powers off");
	ok(!sh.syncs && !sh.reboots, "signalling pid 1 neither syncs nor calls reboot(2)");
}

static void test_options(void)
{
	reset();
	expect_signal(run("shutdown", "-r", "now", NULL), SIGTERM, "shutdown -r now");
	reset();
	expect_signal(run("shutdown", "-H", "now", NULL), SIGUSR1, "shutdown -H now");
	reset();
	expect_signal(run("shutdown", "-h", "now", NULL), SIGUSR2, "shutdown -h now");
	reset();
	expect_signal(run("shutdown", "-P", "now", NULL), SIGUSR2, "shutdown -P now");
	reset();
	expect_signal(run("reboot", "-p", NULL), SIGUSR2, "reboot -p powers off");
	reset();
	expect_signal(run("halt", "-r", NULL), SIGTERM, "halt -r reboots");
	reset();
	expect_signal(run("poweroff", "-H", NULL), SIGUSR1, "poweroff -H halts");
	reset();
	expect_signal(run("shutdown", "-r", "-H", "now", NULL), SIGUSR1, "the last action option wins");
	reset();
	expect_signal(run("shutdown", "now", "-r", NULL), SIGTERM, "an option after TIME still applies");
	reset();
	expect_signal(run("shutdown", "-t", "5", "now", NULL), SIGUSR2, "-t takes its argument");
	reset();
	expect_signal(run("shutdown", "-k", "-n", "-w", "-d", "now", NULL), SIGUSR2,
		      "-k -n -w -d are ignored");
	reset();
	expect_signal(run("reboot", "-t", NULL), SIGTERM, "-t without an argument is ignored");
	reset();
	expect_signal(run("shutdown", "+0", "system", "going", "down", NULL), SIGUSR2,
		      "operands after TIME are ignored");
	reset();
	expect_signal(run("reboot", "now", NULL), SIGTERM, "reboot takes an optional TIME");
}

static void test_errors(void)
{
	int rc;

	reset();
	rc = run("shutdown", NULL);
	is_int(rc, 2, "shutdown without TIME is a usage error");
	has_str(err, "shutdown: missing time operand\n", "and says why");
	has_str(err, "usage: shutdown [OPTION]... TIME\n", "and prints the usage");
	is_int(sh.kills, 0, "and signals nothing");

	reset();
	rc = run("ninit-shutdown", "-r", NULL);
	is_int(rc, 2, "ninit-shutdown without TIME is a usage error");
	has_str(err, "ninit-shutdown: missing time operand\n", "and says why");

	reset();
	rc = run("telinit", NULL);
	is_int(rc, 2, "telinit without a runlevel is a usage error");
	has_str(err, "telinit: missing runlevel operand\n", "and says why");

	reset();
	rc = run("telinit", "q", NULL);
	is_int(rc, 0, "telinit q succeeds");
	is_int(sh.kills, 0, "and does nothing");
	reset();
	rc = run("telinit", "Q", NULL);
	is_int(rc + sh.kills, 0, "telinit Q does nothing either");

	for (const char *const *lv = (const char *const[]){ "1", "2", "3", "5", "s", "06", NULL }; *lv; lv++) {
		char want[96];

		reset();
		rc = run("telinit", *lv, NULL);
		snprintf(want, sizeof(want), "telinit: unknown runlevel '%s', expected 0 or 6\n", *lv);
		ok(rc == 2 && strstr(err, want) && !sh.kills, "telinit %s is a usage error", *lv);
	}

	reset();
	rc = run("reboot", "-x", NULL);
	is_int(rc, 2, "an unknown option is a usage error");
	has_str(err, "reboot: unrecognized option '-x'\n", "and is named");
	has_str(err, "usage: reboot [OPTION]...\n", "with the usage on stderr");

	reset();
	rc = run("shutdown", "-c", NULL);
	is_int(rc, 1, "-c is refused");
	has_str(err, "a pending shutdown runs in the foreground, interrupt it instead", "with the reason");
	is_int(sh.kills, 0, "and nothing is signalled");

	static const char *const bad[] = {
		"+", "+-1", "+x", "+5m", "25:00", "24:00", "12:60", "12:", ":30", "noon", "1200",
		"+99999999999999999999", "+153722867280912931", "12:30:00", "-1:00",
		"+ 5", "++5", "12:+5", "12:-5", " 12:00", "12: 5",
	};
	for (size_t k = 0; k < sizeof(bad) / sizeof(*bad); k++) {
		char want[96];

		reset();
		rc = run("shutdown", bad[k], NULL);
		snprintf(want, sizeof(want), "shutdown: invalid time '%s'\n", bad[k]);
		if (bad[k][0] == '-')
			ok(rc == 2 && !sh.kills, "TIME %s is a usage error", bad[k]);
		else
			ok(rc == 2 && strstr(err, want) && !sh.kills, "TIME %s is a usage error", bad[k]);
	}
}

static void test_time(void)
{
	int rc;

	reset();
	rc = run("shutdown", "+5", NULL);
	is_int(rc, 0, "+5 succeeds");
	is_int(sh.slept, 300, "+5 waits 300 s");
	has_str(out, "shutdown: poweroff in 300 seconds\n", "the delay is printed");
	has_str(con, "\r\nThe system is going down for poweroff in 5 minutes\r\n", "and broadcast to the console");
	is_int(sh.kill_sig, SIGUSR2, "then pid 1 is signalled");

	reset();
	run("shutdown", "-r", "+1", NULL);
	has_str(con, "going down for reboot in 1 minute\r\n", "one minute is singular");
	is_int(sh.slept, 60, "+1 waits 60 s");

	reset();
	run("shutdown", "-H", "12:01", NULL);
	is_int(sh.slept, 30, "12:01 at 12:00:30 waits 30 s");
	has_str(con, "going down for halt in 30 seconds\r\n", "under a minute is told in seconds");

	reset();
	run("shutdown", "12:00", NULL);
	is_int(sh.slept, 86370, "a time just passed means tomorrow");
	has_str(con, "in 1439 minutes", "and is broadcast in minutes");

	reset();
	run("shutdown", "0:0", NULL);
	is_int(sh.slept, 12 * 3600 - 30, "0:0 is the next midnight");

	reset();
	sh.now = 12 * 3600 + 59;
	run("shutdown", "12:01", NULL);
	has_str(con, "in 1 second\r\n", "one second is singular");

	reset();
	run("shutdown", "+0", NULL);
	is_int(sh.sleep_calls, 0, "+0 does not wait");
	is_str(con, "", "+0 broadcasts nothing");

	reset();
	run("shutdown", "now", NULL);
	is_int(sh.sleep_calls + (int)strlen(con) + (int)strlen(out), 0, "now neither waits nor prints");

	reset();
	sh.sleep_short = 3;
	run("shutdown", "+2", NULL);
	is_int(sh.slept, 120, "an interrupted sleep is resumed for the remainder");
	ok(sh.sleep_calls > 1, "over several calls");

	reset();
	sh.comm = "openrc-init\n";
	run("shutdown", "+5", NULL);
	is_int(sh.slept, 0, "on another init nothing is waited on before delegating");
}

static void test_force(void)
{
	static const struct {
		const char *argv0, *opt;
		int cmd;
	} v[] = {
		{ "poweroff", "-f", RB_POWER_OFF },
		{ "reboot", "-f", RB_AUTOBOOT },
		{ "halt", "-f", RB_HALT_SYSTEM },
		{ "shutdown", "-r", RB_AUTOBOOT },
	};
	int rc;

	for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		reset();
		if (!strcmp(v[k].argv0, "shutdown"))
			rc = run(v[k].argv0, "-f", v[k].opt, "now", NULL);
		else
			rc = run(v[k].argv0, v[k].opt, NULL);
		ok(sh.syncs == 1 && sh.reboots == 1 && sh.reboot_cmd == v[k].cmd,
		   "%s -f syncs and calls reboot(2) with %#x", v[k].argv0, (unsigned)v[k].cmd);
		ok(!sh.kills, "%s -f does not signal pid 1", v[k].argv0);
		ok(rc == 1 && strstr(err, ": reboot: Operation not permitted\n"),
		   "%s -f reports a refused reboot(2)", v[k].argv0);
	}

	reset();
	sh.slept = 0;
	run("shutdown", "-f", "+5", NULL);
	is_int(sh.sleep_calls, 0, "-f does not wait for TIME");
}

static void test_delegate(void)
{
	int rc;

	reset();
	sh.comm = "systemd\n";
	rc = run("reboot", "--no-wall", NULL);
	is_int(sh.execs, 1, "on another init NAME.old is executed");
	is_str(sh.exec_path, "/usr/sbin/reboot.old", "from the directory of the running binary");
	ok(sh.exec_argc == 2 && !strcmp(sh.exec_argv[0], "reboot") &&
	   !strcmp(sh.exec_argv[1], "--no-wall"),
	   "with argv[0] set to its name and options it does not know passed on");
	is_int(sh.kills, 0, "pid 1 is not signalled");
	is_int(rc, 1001, "a failed exec exits 1");
	has_str(err, "reboot: pid 1 is not ninit and /usr/sbin/reboot.old: No such file or directory\n",
		"and says why");

	reset();
	sh.comm = "init\n";
	run("/sbin/poweroff", "-f", NULL);
	ok(sh.execs == 1 && !sh.reboots && !sh.syncs, "-f is delegated as well, before any reboot(2)");
	is_str(sh.exec_path, "/usr/sbin/poweroff.old", "poweroff.old is the target");

	reset();
	sh.comm = "openrc-init\n";
	run("shutdown", NULL);
	ok(sh.execs == 1, "shutdown without TIME is delegated, the other shutdown decides");

	reset();
	sh.comm = "runit\n";
	run("halt", "--help", NULL);
	ok(sh.execs == 1, "--help is delegated");

	reset();
	sh.comm = "systemd\n";
	sh.self = NULL;
	run("telinit", "3", NULL);
	ok(sh.execs == 1, "telinit with a runlevel ninit lacks is delegated");
	is_str(sh.exec_path, NINIT_SBINDIR "/telinit.old", "without /proc/self/exe sbindir is used");

	reset();
	sh.comm = "ninit";
	expect_signal(run("reboot", NULL), SIGTERM, "comm without a newline is still ninit");
	reset();
	sh.comm = NULL;
	expect_signal(run("reboot", NULL), SIGTERM, "an unreadable /proc/1/comm is taken as ninit");
	ok(!sh.execs, "and nothing is delegated");
	reset();
	sh.comm = "ninit-real\n";
	run("reboot", NULL);
	is_int(sh.execs, 1, "a comm that only starts with ninit is another init");
}

static void test_user(void)
{
	static const struct {
		const char *argv0, *method;
	} v[] = {
		{ "poweroff", "org.freedesktop.login1.Manager.PowerOff" },
		{ "reboot", "org.freedesktop.login1.Manager.Reboot" },
		{ "halt", "org.freedesktop.login1.Manager.Halt" },
	};
	int rc;

	for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		reset();
		sh.euid = 1000;
		sh.fork_ret = 0;
		rc = run(v[k].argv0, NULL);
		is_int(rc, 2127, "the dbus-send child exits 127 when it cannot exec");
		ok(sh.lp_argc == 7 && !strcmp(sh.lp_args[0], "dbus-send") &&
		   !strcmp(sh.lp_args[1], "dbus-send") && !strcmp(sh.lp_args[2], "--system") &&
		   !strcmp(sh.lp_args[3], "--dest=org.freedesktop.login1") &&
		   !strcmp(sh.lp_args[4], "/org/freedesktop/login1") &&
		   !strcmp(sh.lp_args[5], v[k].method),
		   "%s as a user calls %s", v[k].argv0, v[k].method);
		ok(!sh.kills, "%s as a user does not signal pid 1", v[k].argv0);
	}
	reset();
	sh.euid = 1000;
	sh.fork_ret = 0;
	run("reboot", NULL);
	ok(sh.lp_argc == 7 && !strcmp(sh.lp_args[6], "boolean:true"),
	   "dbus-send requests an interactive authorization check");

	reset();
	sh.euid = 1000;
	sh.wait_status = 0;
	sh.wait_eintr = 2;
	rc = run("reboot", NULL);
	is_int(rc, 0, "logind accepting the request is success");
	is_int(sh.kills, 0, "and pid 1 is left alone");

	reset();
	sh.euid = 1000;
	sh.wait_status = 1 << 8;
	rc = run("reboot", NULL);
	is_int(rc, 1, "logind refusing is a failure");
	has_str(err, "reboot: permission denied, must be run as root\n", "reported as permission denied");

	reset();
	sh.euid = 1000;
	sh.fork_ret = -1;
	rc = run("reboot", NULL);
	is_int(rc, 1, "a fork failure falls back to permission denied");

	reset();
	sh.kill_errno = EPERM;
	rc = run("reboot", NULL);
	is_int(rc, 1, "a refused kill fails");
	has_str(err, "reboot: cannot signal pid 1: Operation not permitted\n", "and says why");
}

static void test_usage(void)
{
	int rc;

	reset();
	rc = run("shutdown", "--help", NULL);
	is_int(rc, 0, "--help exits 0");
	has_str(out, "usage: shutdown [OPTION]... TIME\n", "shutdown's usage names TIME");
	has_str(out, "TIME is now, +MINUTES or HH:MM\n", "and its forms");
	is_int(sh.kills, 0, "--help does nothing else");

	reset();
	run("reboot", "--help", NULL);
	has_str(out, "usage: reboot [OPTION]...\n", "reboot's usage has no TIME");

	reset();
	run("ninit-shutdown", "--help", NULL);
	has_str(out, "usage: ninit-shutdown [OPTION]... TIME\n", "ninit-shutdown requires TIME and says so");

	reset();
	rc = run("shutdown", "now", "--help", NULL);
	is_int(sh.kills + rc, 0, "--help anywhere wins over the action");
}

int main(void)
{
	int fd;

	setenv("TZ", "UTC", 1);
	tzset();
	fd = mkstemp(con_path);
	close(fd);

	test_names();
	test_options();
	test_errors();
	test_time();
	test_force();
	test_delegate();
	test_user();
	test_usage();
	unlink(con_path);
	return tap_done();
}
