#include "fail.h"
#include "logging.h"
#include "ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
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

static const char *plural_s(unsigned n)
{
	return n == 1 ? "" : "s";
}

static const char *verb_s(unsigned n)
{
	return n == 1 ? "s" : "";
}

static const char *const signame[] = {
	[SIGHUP] = "HUP", [SIGINT] = "INT", [SIGQUIT] = "QUIT", [SIGILL] = "ILL",
	[SIGTRAP] = "TRAP", [SIGABRT] = "ABRT", [SIGBUS] = "BUS", [SIGFPE] = "FPE",
	[SIGKILL] = "KILL", [SIGUSR1] = "USR1", [SIGSEGV] = "SEGV", [SIGUSR2] = "USR2",
	[SIGPIPE] = "PIPE", [SIGALRM] = "ALRM", [SIGTERM] = "TERM", [SIGCHLD] = "CHLD",
	[SIGCONT] = "CONT", [SIGSTOP] = "STOP", [SIGTSTP] = "TSTP", [SIGTTIN] = "TTIN",
	[SIGTTOU] = "TTOU", [SIGURG] = "URG", [SIGXCPU] = "XCPU", [SIGXFSZ] = "XFSZ",
	[SIGVTALRM] = "VTALRM", [SIGPROF] = "PROF", [SIGWINCH] = "WINCH", [SIGIO] = "IO",
	[SIGPWR] = "PWR", [SIGSYS] = "SYS",
#ifdef SIGSTKFLT
	[SIGSTKFLT] = "STKFLT",
#endif
};

static void describe(int status, char *buf, size_t cap)
{
	if (status == FAIL_ST_NOTIFY_HUP)
		snprintf(buf, cap, "closed its notify fd before reporting ready");
	else if (status == FAIL_ST_TIMEOUT)
		snprintf(buf, cap, "timed out and was killed");
	else if (WIFEXITED(status))
		snprintf(buf, cap, "exit %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status)) {
		unsigned sig = (unsigned)WTERMSIG(status);
		const char *ab = sig < sizeof(signame) / sizeof(*signame) ? signame[sig] : NULL;
		const char *core = WCOREDUMP(status) ? " (core dumped)" : "";

		if (ab)
			snprintf(buf, cap, "killed by SIG%s%s", ab, core);
		else
			snprintf(buf, cap, "killed by signal %d%s", WTERMSIG(status), core);
	}
	else
		snprintf(buf, cap, "status %#x", (unsigned)status);
}

void fail_describe(int status, char *buf, size_t cap)
{
	describe(status, buf, cap);
}

enum fail_act fail_service(const void *map, uint32_t i, int status, unsigned attempt,
			   const char *tail, size_t tail_len)
{
	const struct ng_svc *s = &ng_svcs(map)[i];
	const char *name = ng_name(map, i);
	char how[64];

	describe(status, how, sizeof(how));

	if (attempt < 2) {
		log_warn("%s: failed (%s), retrying", name, how);
		return FAIL_RETRY;
	}

	log_err("%s: failed twice (%s)", name, how);
	if (tail_len)
		log_raw(LOG_FAIL, tail, tail_len);

	switch (ng_onfail(map, i)) {
	case NG_ONFAIL_WARN:
		log_warn("%s: nothing depends on it, continuing without it", name);
		return FAIL_WARN;
	case NG_ONFAIL_SHELL:
		log_err("%s: %u of %u services depend%s on it, dropping to a shell", name,
			s->n_desc, ((const struct ng_hdr *)map)->n_svc, verb_s(s->n_desc));
		return FAIL_SHELL;
	default:
		log_err("%s: %u service%s depend%s on it, stopping", name, s->n_desc,
			plural_s(s->n_desc), verb_s(s->n_desc));
		return FAIL_STOP;
	}
}

uint32_t fail_poison(const void *map, uint32_t i, uint8_t *state)
{
	const uint32_t *roff = ng_rdep_off(map), *ridx = ng_rdep_idx(map);
	uint32_t n = ((const struct ng_hdr *)map)->n_svc;
	uint32_t j, k, count = 0;

	state[i] = NG_ST_FAILED;

	for (j = i; j < n; j++) {
		if (state[j] != NG_ST_FAILED && state[j] != NG_ST_SKIPPED)
			continue;
		for (k = roff[j]; k < roff[j + 1]; k++) {
			uint32_t d = ridx[k];
			if (state[d] == NG_ST_PENDING) {
				state[d] = NG_ST_SKIPPED;
				count++;
			}
		}
	}

	return count;
}

#define EMERG_FAST_MS	 1000
#define EMERG_FAST_MAX	 5
#define EMERG_FAIL_MAX	 5
#define EMERG_NOEXEC_MAX 2
#define EMERG_EXEC_MS	 5000
#define EMERG_NOEXEC	 127

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1u << 2)
#endif

static void cloexec_range(unsigned lo, unsigned hi)
{
	struct rlimit rl;
	unsigned fd, max;

#ifdef SYS_close_range
	if (syscall(SYS_close_range, lo, hi, CLOSE_RANGE_CLOEXEC) == 0)
		return;
#endif
	max = getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
	      rl.rlim_cur < 65536 ? (unsigned)rl.rlim_cur : 65536;
	if (hi != ~0u && hi + 1 < max)
		max = hi + 1;
	for (fd = lo; fd < max; fd++)
		fcntl((int)fd, F_SETFD, FD_CLOEXEC);
}

void ninit_oom_score_adj(const char *v, size_t len)
{
	int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	(void)!write(fd, v, len);
	close(fd);
}

void ninit_cloexec_except(int keep)
{
	if (keep < 3) {
		cloexec_range(3, ~0u);
		return;
	}
	if (keep > 3)
		cloexec_range(3, (unsigned)keep - 1);
	cloexec_range((unsigned)keep + 1, ~0u);
}

static pid_t emerg_pid = -1;
static struct timespec emerg_at;
static unsigned emerg_fast;
static unsigned emerg_fail;
static int emerg_gone;
static int emerg_execed;
static int emerg_greeted;
static unsigned emerg_noexec;

static int emerg_console(void)
{
	int fd = open("/dev/console", O_RDWR | O_NOCTTY | O_CLOEXEC);
	int err;

	if (fd >= 0 && fd < 3) {
		int up = fcntl(fd, F_DUPFD_CLOEXEC, 3);

		if (up >= 0) {
			close(fd);
			fd = up;
		}
	}
	if (fd >= 0)
		return fd;

	// the kernel hands pid 1 the console on 0..2 and that is the only fallback
	err = errno;
	if (isatty(STDIN_FILENO))
		fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
	if (fd < 0)
		errno = err;
	return fd;
}

// whatever died may have left the console unusable
static int emerg_console_reset(int con)
{
	struct termios t;
	char kbtype;
	int mode, vt = ioctl(con, KDGKBTYPE, &kbtype) == 0;

	if (vt) {
		if (ioctl(con, KDGETMODE, &mode) == 0 && mode != KD_TEXT) {
			log_warn("console: in graphics mode, restoring text");
			ioctl(con, KDSETMODE, KD_TEXT);
		}
		if (ioctl(con, KDGKBMODE, &mode) == 0 && mode != K_XLATE && mode != K_UNICODE) {
			log_warn("console: keyboard in raw mode, restoring");
			ioctl(con, KDSKBMODE, K_UNICODE);
		}
	}

	if (tcgetattr(con, &t) == 0) {
		const tcflag_t sane = ISIG | ICANON | ECHO;

		if ((t.c_lflag & sane) != sane || !(t.c_oflag & OPOST))
			log_warn("console: terminal in raw mode, restoring");
		t.c_iflag = ICRNL | IXON | IMAXBEL | BRKINT | IUTF8;
		t.c_oflag = OPOST | ONLCR;
		t.c_cflag |= CREAD;
		t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
		t.c_cc[VINTR] = 003;
		t.c_cc[VQUIT] = 034;
		t.c_cc[VERASE] = 0177;
		t.c_cc[VWERASE] = 027;
		t.c_cc[VREPRINT] = 022;
		t.c_cc[VLNEXT] = 026;
		t.c_cc[VKILL] = 025;
		t.c_cc[VEOF] = 004;
		t.c_cc[VSTART] = 021;
		t.c_cc[VSTOP] = 023;
		t.c_cc[VSUSP] = 032;
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		tcsetattr(con, TCSANOW, &t);
		tcflush(con, TCIFLUSH);
	}

	return vt;
}

static pid_t emerg_spawn(int con, int vt, int barrier)
{
	static char arg0[] = "-sh", path[] = NG_PATH, home[] = "HOME=/";
	static char term_vt[] = "TERM=linux", term_serial[] = "TERM=vt220";
	static char *const argv[] = { arg0, NULL };
	static char *const env_vt[] = { path, term_vt, home, NULL };
	static char *const env_serial[] = { path, term_serial, home, NULL };
	char *const *envp = vt ? env_vt : env_serial;
	struct sigaction dfl = { .sa_handler = SIG_DFL };
	sigset_t none;
	pid_t pid;
	int sig, errs[2] = { 0, 0 };

	pid = fork();
	if (pid != 0)
		return pid;

	// only async-signal-safe calls from here to execve
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (sig = 1; sig < NSIG; sig++)
		sigaction(sig, &dfl, NULL);

	setsid();
	ioctl(con, TIOCSCTTY, 1);
	dup2(con, 0);
	dup2(con, 1);
	dup2(con, 2);

	ninit_cloexec_except(-1);

	// oom exemption must not reach whatever is run from this shell
	ninit_oom_score_adj("0\n", 2);

	// pid 1 may have dropped umask to 0
	(void)!chdir("/");
	umask(022);

#ifdef NINIT_BUSYBOX
	execve(NINIT_BUSYBOX, argv, envp);
	errs[0] = errno;
#endif
	execve("/bin/sh", argv, envp);
	errs[1] = errno;
	(void)!write(barrier, errs, sizeof(errs));
	_exit(EMERG_NOEXEC);
}

static int emerg_confirm(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int errs[2] = { 0, 0 }, n;

	do
		n = poll(&pfd, 1, EMERG_EXEC_MS);
	while (n < 0 && errno == EINTR);
	if (n <= 0)
		return 0;

	do
		n = (int)read(fd, errs, sizeof(errs));
	while (n < 0 && errno == EINTR);
	if (n != (int)sizeof(errs))
		return 0;

#ifdef NINIT_BUSYBOX
	log_warn("shell: exec %s: %s", NINIT_BUSYBOX, strerror(errs[0]));
#endif
	log_err("shell: exec /bin/sh: %s", strerror(errs[1]));
	return errs[1];
}

static int emerg_start(void)
{
	int bar[2], con, err, vt;
	pid_t pid;

	emerg_pid = -1;

	con = emerg_console();
	if (con < 0) {
		log_err("shell: no console available: %s", strerror(errno));
		return -1;
	}
	if (pipe2(bar, O_CLOEXEC)) {
		log_err("shell: pipe: %s", strerror(errno));
		close(con);
		return -1;
	}

	vt = emerg_console_reset(con);
	pid = emerg_spawn(con, vt, bar[1]);
	err = errno;
	close(con);
	close(bar[1]);
	if (pid < 0) {
		close(bar[0]);
		log_err("shell: fork: %s", strerror(err));
		return -1;
	}

	clock_gettime(CLOCK_MONOTONIC, &emerg_at);

	emerg_pid = pid;

	err = emerg_confirm(bar[0]);
	close(bar[0]);
	emerg_execed = !err;
	if (err)
		log_err("shell: no working shell on this machine: %s", strerror(err));

	return 0;
}

static long long emerg_uptime_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)(now.tv_sec - emerg_at.tv_sec) * 1000 +
	       (now.tv_nsec - emerg_at.tv_nsec) / 1000000;
}

static void emerg_restart(void)
{
	if (emerg_start() == 0) {
		if (!emerg_execed) {
			if (++emerg_noexec >= EMERG_NOEXEC_MAX) {
				emerg_gone = 1;
				log_err("shell: none can be started here, reboot with sysrq or power-cycle");
			}
			return;
		}
		emerg_fail = 0;
		emerg_noexec = 0;
		if (!emerg_greeted) {
			emerg_greeted = 1;
			ninit_log(LOG_DONE, "shell: running on the console, use reboot or poweroff to exit");
			log_note("shell: you're on your own now, good luck");
		}
		return;
	}
	if (++emerg_fail >= EMERG_FAIL_MAX) {
		emerg_gone = 1;
		log_err("shell: gave up after %u attempts, the console is unattended", emerg_fail);
		return;
	}
	log_err("shell: not running, the console is unattended");
}

void fail_emergency_shell(const char *why)
{
	log_err("%s", why);

	if (emerg_gone)
		return;
	if (emerg_pid > 0) {
		log_warn("shell: already running");
		return;
	}

	emerg_restart();
}

int fail_emergency_reaped(pid_t pid, int status)
{
	long long ms;
	char how[64];

	if (pid <= 0 || pid != emerg_pid)
		return 0;

	emerg_pid = -1;
	ms = emerg_uptime_ms();
	describe(status, how, sizeof(how));

	if (ms >= EMERG_FAST_MS) {
		emerg_fast = 0;
	} else if (++emerg_fast >= EMERG_FAST_MAX) {
		emerg_gone = 1;
		log_err("shell: died immediately %u times (%s), not restarting",
			emerg_fast, how);
		log_err("shell: none can be started here, reboot with sysrq or power-cycle");
		return 1;
	}

	if (!(WIFEXITED(status) && WEXITSTATUS(status) == EMERG_NOEXEC))
		log_warn("shell: exited (%s)", how);

	if (emerg_gone)
		return 1;

	emerg_restart();
	return 1;
}

void fail_summary(const void *map, const uint8_t *state)
{
	uint32_t n = ((const struct ng_hdr *)map)->n_svc;
	uint32_t i, failed = 0, skipped = 0;

	for (i = 0; i < n; i++) {
		failed += state[i] == NG_ST_FAILED;
		skipped += state[i] == NG_ST_SKIPPED;
	}
	if (!failed && !skipped)
		return;

	ninit_log(failed ? LOG_FAIL : LOG_WARN, "boot: %u service%s failed, %u never started",
		  failed, plural_s(failed), skipped);
	for (i = 0; i < n; i++)
		if (state[i] == NG_ST_FAILED)
			log_err("boot: %-7s %s", "failed", ng_name(map, i));
	for (i = 0; i < n; i++)
		if (state[i] == NG_ST_SKIPPED)
			log_warn("boot: %-7s %s", "skipped", ng_name(map, i));
}
