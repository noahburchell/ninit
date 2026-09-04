#include "fail.h"
#include "logging.h"
#include "ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void describe(int status, char *buf, size_t cap)
{
	if (WIFEXITED(status))
		snprintf(buf, cap, "exit %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status)) {
		// no abbreviation for realtime signals
		const char *ab = sigabbrev_np(WTERMSIG(status));
		const char *core = WCOREDUMP(status) ? " (core dumped)" : "";

		if (ab)
			snprintf(buf, cap, "killed by SIG%s%s", ab, core);
		else
			snprintf(buf, cap, "killed by signal %d%s", WTERMSIG(status), core);
	}
	else
		snprintf(buf, cap, "status %#x", (unsigned)status);
}

enum fail_act fail_service(const void *map, uint32_t i, int status, unsigned attempt,
			   const char *tail, size_t tail_len)
{
	const struct ng_svc *s = &ng_svcs(map)[i];
	const char *name = ng_name(map, i);
	char how[64];

	describe(status, how, sizeof(how));

	if (attempt < 2) {
		log_warn("%s failed (%s), retrying once", name, how);
		return FAIL_RETRY;
	}

	log_err("%s failed twice (%s)", name, how);
	if (tail_len)
		log_raw(LOG_ERR, tail, tail_len);

	switch (ng_onfail(map, i)) {
	case NG_ONFAIL_WARN:
		log_warn("nothing depends on %s. continuing without it", name);
		return FAIL_WARN;
	case NG_ONFAIL_SHELL:
		log_err("%u of %u services depend on %s", s->n_desc,
			((const struct ng_hdr *)map)->n_svc, name);
		return FAIL_SHELL;
	default:
		log_err("%u service(s) depend on %s", s->n_desc, name);
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

#define EMERG_FAST_MS	1000
#define EMERG_FAST_MAX	5
#define EMERG_FAIL_MAX	5

static char emerg_path[] = NG_PATH;

static pid_t emerg_pid = -1;
static struct timespec emerg_at;
static unsigned emerg_fast;
static unsigned emerg_fail;
static int emerg_gone;

static int emerg_console(void)
{
	int fd = open("/dev/console", O_RDWR | O_NOCTTY | O_CLOEXEC);
	int err;

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

// close_range() shut the log fd, but 0..2 are the console, so report by hand
static void emerg_execfail(const char *path)
{
	char msg[192];
	int n = snprintf(msg, sizeof(msg), "ninit: exec %s: %s\n", path, strerror(errno));

	if (n > 0)
		(void)!write(STDERR_FILENO, msg, n < (int)sizeof(msg) ? (size_t)n : sizeof(msg));
}

static pid_t emerg_spawn(int con)
{
	const char *term;
	char kbtype;
	sigset_t none;
	pid_t pid;
	int sig;

	pid = fork();
	if (pid != 0)
		return pid;

	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (sig = 1; sig < NSIG; sig++)
		signal(sig, SIG_DFL);

	setsid();
	ioctl(con, TIOCSCTTY, 1);
	// KDGKBTYPE answers on a virtual terminal and fails on a serial line
	term = ioctl(con, KDGKBTYPE, &kbtype) == 0 ? "linux" : "vt220";
	dup2(con, 0);
	dup2(con, 1);
	dup2(con, 2);

	close_range(3, ~0u, 0);

	// pid 1 may have dropped umask to 0
	(void)!chdir("/");
	umask(022);

	putenv(emerg_path);
	setenv("TERM", term, 1);

#ifdef NINIT_BUSYBOX
	execl(NINIT_BUSYBOX, "-sh", (char *)NULL);
	emerg_execfail(NINIT_BUSYBOX);
#endif
	execl("/bin/sh", "-sh", (char *)NULL);
	emerg_execfail("/bin/sh");
	_exit(127);
}

static pid_t emerg_start(void)
{
	pid_t pid;
	int con, err;

	con = emerg_console();
	if (con < 0) {
		log_err("no console for emergency shell: %s", strerror(errno));
		return -1;
	}

	pid = emerg_spawn(con);
	err = errno;
	close(con);
	if (pid < 0) {
		log_err("fork for emergency shell: %s", strerror(err));
		return -1;
	}

	clock_gettime(CLOCK_MONOTONIC, &emerg_at);
	return pid;
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
	emerg_pid = emerg_start();
	if (emerg_pid > 0) {
		emerg_fail = 0;
		return;
	}
	if (++emerg_fail >= EMERG_FAIL_MAX) {
		emerg_gone = 1;
		log_err("no emergency shell after %u attempts. giving up on the console", emerg_fail);
		return;
	}
	log_err("no emergency shell is running. the console is unattended");
}

void fail_emergency_shell(const char *why)
{
	log_err("%s", why);

	if (emerg_gone)
		return;
	if (emerg_pid > 0) {
		log_err("an emergency shell is already running");
		return;
	}

	log_err("starting an emergency shell");
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
		log_err("emergency shell died immediately %u times (%s). not restarting",
			emerg_fast, how);
		log_err("no usable shell on this machine. reboot with sysrq or power-cycle");
		return 1;
	}

	log_warn("emergency shell exited (%s). the machine is still in a failed state", how);
	log_warn("you're on your own. run shutdown, reboot or poweroff");

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

	log_warn("%u service(s) failed, %u never started", failed, skipped);
	for (i = 0; i < n; i++)
		if (state[i] == NG_ST_FAILED)
			log_err("  failed:  %s", ng_name(map, i));
	for (i = 0; i < n; i++)
		if (state[i] == NG_ST_SKIPPED)
			log_warn("  skipped: %s", ng_name(map, i));
}
