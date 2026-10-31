#include "fail.h"
#include "logging.h"
#include "ngraph.h"
#include "run.h"
#include "../help/time.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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

#define NG_KDGKBTYPE   0x4B33
#define NG_KDSETMODE   0x4B3A
#define NG_KDGETMODE   0x4B3B
#define NG_KDGKBMODE   0x4B44
#define NG_KDSKBMODE   0x4B45
#define NG_KD_TEXT     0x00
#define NG_K_XLATE     0x01
#define NG_K_UNICODE   0x03
#define NG_VT_ACTIVATE 0x5606

static const char* verb_s(uint16_t n) {
	return n == 1 ? "s" : "";
}

static const char* const signame[] = {
	[SIGHUP] = "HUP",
	[SIGINT] = "INT",
	[SIGQUIT] = "QUIT",
	[SIGILL] = "ILL",
	[SIGTRAP] = "TRAP",
	[SIGABRT] = "ABRT",
	[SIGBUS] = "BUS",
	[SIGFPE] = "FPE",
	[SIGKILL] = "KILL",
	[SIGUSR1] = "USR1",
	[SIGSEGV] = "SEGV",
	[SIGUSR2] = "USR2",
	[SIGPIPE] = "PIPE",
	[SIGALRM] = "ALRM",
	[SIGTERM] = "TERM",
	[SIGCHLD] = "CHLD",
	[SIGCONT] = "CONT",
	[SIGSTOP] = "STOP",
	[SIGTSTP] = "TSTP",
	[SIGTTIN] = "TTIN",
	[SIGTTOU] = "TTOU",
	[SIGURG] = "URG",
	[SIGXCPU] = "XCPU",
	[SIGXFSZ] = "XFSZ",
	[SIGVTALRM] = "VTALRM",
	[SIGPROF] = "PROF",
	[SIGWINCH] = "WINCH",
	[SIGIO] = "IO",
	[SIGPWR] = "PWR",
	[SIGSYS] = "SYS",
#ifdef SIGSTKFLT
	[SIGSTKFLT] = "STKFLT",
#endif
};

void fail_describe(int status, char* buf, size_t cap) {
	if(status == FAIL_ST_NOTIFY_HUP) {
		snprintf(buf, cap, "closed its notify fd before reporting ready");
	} else if(status == FAIL_ST_TIMEOUT) {
		snprintf(buf, cap, "timed out and was killed");
	} else if(WIFEXITED(status)) {
		snprintf(buf, cap, "exit %d", WEXITSTATUS(status));
	} else if(WIFSIGNALED(status)) {
		uint8_t sig = (uint8_t)WTERMSIG(status);
		const char* ab = sig < sizeof(signame) / sizeof(*signame) ? signame[sig] : NULL;
		const char* core = WCOREDUMP(status) ? " (core dumped)" : "";

		if(ab) {
			snprintf(buf, cap, "killed by SIG%s%s", ab, core);
		} else {
			snprintf(buf, cap, "killed by signal %d%s", WTERMSIG(status), core);
		}
	} else {
		snprintf(buf, cap, "status %#x", (uint32_t)status);
	}
}

enum fail_act fail_service(const void* map, uint32_t i, int status, uint8_t attempt,
			   uint8_t tries, str tail) {
	const struct ng_svc* s = &ng_svcs(map)[i];
	const char* name = ng_name(map, i);
	char how[64];

	fail_describe(status, how, sizeof(how));

	if(attempt < tries) {
		log_warn("%s: failed (%s), retrying (%" PRIu8 " of %" PRIu8 ")", name, how, attempt, tries);
		return FAIL_RETRY;
	}

	log_err("%s: failed %" PRIu8 " time%s (%s)", name, attempt, plural(attempt), how);
	if(tail.n) {
		log_raw(LOG_FAIL, tail);
	}

	// restart covers supervision after readiness
	if(ng_restart(map, i)) {
		log_warn("%s: restart applies after readiness, start-tries is %" PRIu8, name, tries);
	}

	switch(ng_onfail(map, i)) {
		case NG_ONFAIL_WARN:
			log_warn("%s: no dependent services, continuing", name);
			return FAIL_WARN;
		case NG_ONFAIL_SHELL:
			log_err("%s: %" PRIu16 " of %u services depend%s on it, starting emergency shell", name,
				s->n_desc, ((const struct ng_hdr*)map)->n_svc, verb_s(s->n_desc));
			return FAIL_SHELL;
		default:
			log_err("%s: %" PRIu16 " service%s depend%s on it, stopping", name, s->n_desc,
				plural(s->n_desc), verb_s(s->n_desc));
			return FAIL_STOP;
	}
}

static bool poison_take(const uint8_t* state, const uint8_t* up, const void* map, const struct run* rs, uint32_t d) {
	if(state[d] == NG_ST_PENDING) {
		return true;
	}

	// a start between attempts has no process and cannot go on without the prerequisite
	if(state[d] == NG_ST_RUNNING) {
		return rs && rs[d].restart_at && rs[d].pid <= 0;
	}

	return ng_svcs(map)[d].type == NG_TYPE_TARGET && state[d] == NG_ST_DONE && !up[d];
}

static uint32_t poison_spread(const void* map, uint8_t* restrict state, const uint8_t* restrict up,
			      const struct run* rs, uint32_t from, uint32_t* restrict undone,
			      uint32_t* restrict waiting) {
	const uint32_t* roff = ng_rdep_off(map);
	const uint32_t* ridx = ng_rdep_idx(map);
	uint32_t n = ((const struct ng_hdr*)map)->n_svc;
	uint32_t j, k, count = 0;

	for(j = from; j < n; j++) {
		if(state[j] != NG_ST_FAILED && state[j] != NG_ST_SKIPPED) {
			continue;
		}

		for(k = roff[j]; k < roff[j + 1]; k++) {
			uint32_t d = ridx[k];

			if(!poison_take(state, up, map, rs, d)) {
				continue;
			}

			if(state[d] == NG_ST_DONE) {
				(*undone)++;
			} else {
				count++;
				*waiting += state[d] == NG_ST_RUNNING;
			}

			state[d] = NG_ST_SKIPPED;
		}
	}

	return count;
}

// waiting counts the skipped services that were between start attempts
uint32_t fail_poison(const void* map, uint32_t i, uint8_t* restrict state, const uint8_t* restrict up,
		     const struct run* rs, uint32_t* restrict undone, uint32_t* restrict waiting) {
	*undone = 0;
	*waiting = 0;
	state[i] = NG_ST_FAILED;

	return poison_spread(map, state, up, rs, i, undone, waiting);
}

uint32_t fail_poison_deps(const void* map, uint32_t i, uint8_t* restrict state, const uint8_t* restrict up,
			  const struct run* rs, uint32_t* restrict undone, uint32_t* restrict waiting) {
	const uint32_t* roff = ng_rdep_off(map);
	const uint32_t* ridx = ng_rdep_idx(map);
	uint32_t k, count = 0;

	*undone = 0;
	*waiting = 0;
	for(k = roff[i]; k < roff[i + 1]; k++) {
		uint32_t d = ridx[k];

		if(!poison_take(state, up, map, rs, d)) {
			continue;
		}

		if(state[d] == NG_ST_DONE) {
			(*undone)++;
		} else {
			count++;
			*waiting += state[d] == NG_ST_RUNNING;
		}

		state[d] = NG_ST_SKIPPED;
	}

	if(!count && !*undone) {
		return 0;
	}

	return count + poison_spread(map, state, up, rs, i + 1, undone, waiting);
}

#define EMERG_FAST_MS	 1000
#define EMERG_FAST_MAX	 5
#define EMERG_FAIL_MAX	 5
#define EMERG_NOEXEC_MAX 2
#define EMERG_EXEC_MS	 5000
#define EMERG_RETRY_MS	 2000
#define EMERG_NOEXEC	 127

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1u << 2)
#endif

static void cloexec_range(uint32_t lo, uint32_t hi) {
	struct rlimit rl;
	uint32_t fd, max;

#ifdef SYS_close_range
	if(syscall(SYS_close_range, lo, hi, CLOSE_RANGE_CLOEXEC) == 0) {
		return;
	}
#endif
	max = getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
			      && rl.rlim_cur < 65536
		      ? (uint32_t)rl.rlim_cur
		      : 65536;
	if(hi != ~0u && hi + 1 < max) {
		max = hi + 1;
	}

	for(fd = lo; fd < max; fd++) {
		fcntl((int)fd, F_SETFD, FD_CLOEXEC);
	}
}

void ninit_oom_score_adj(str v) {
	int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);

	if(fd < 0) {
		return;
	}

	(void)!write(fd, v.p, v.n);
	close(fd);
}

void ninit_cloexec_except(int keep) {
	if(keep < 3) {
		cloexec_range(3, ~0u);
		return;
	}

	if(keep > 3) {
		cloexec_range(3, (uint32_t)keep - 1);
	}

	cloexec_range((uint32_t)keep + 1, ~0u);
}

enum emerg_state {
	EMERG_EXEC_FAILED = -1,
	EMERG_EXEC_UNKNOWN = 0,
	EMERG_EXEC_OK = 1,
};

static pid_t emerg_pid = -1;
static int emerg_bar = -1;
static int64_t emerg_conf_at;
static struct timespec emerg_at;
static uint8_t emerg_fast;
static uint8_t emerg_fail;
static bool emerg_gone;
static enum emerg_state emerg_conf;
static bool emerg_greeted;
static uint8_t emerg_noexec;
static int64_t emerg_retry_at;

static int emerg_console(void) {
	int fd = open("/dev/console", O_RDWR | O_NOCTTY | O_CLOEXEC);
	int err;

	if(fd >= 0 && fd < 3) {
		int up = fcntl(fd, F_DUPFD_CLOEXEC, 3);

		if(up >= 0) {
			close(fd);
			fd = up;
		}
	}

	if(fd >= 0) {
		return fd;
	}

	// the kernel hands pid 1 the console on 0..2 and that is the only fallback
	err = errno;
	if(isatty(STDIN_FILENO)) {
		fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
	}

	if(fd < 0) {
		errno = err;
	}

	return fd;
}

// a compositor that died leaves its vt in graphics mode and the keyboard raw, 1 for a vt
static bool console_text(int con) {
	char kbtype;
	int mode;

	if(ioctl(con, NG_KDGKBTYPE, &kbtype) < 0) {
		return false;
	}

	if(ioctl(con, NG_KDGETMODE, &mode) == 0 && mode != NG_KD_TEXT) {
		log_warn("console: in graphics mode, restoring text");
		ioctl(con, NG_KDSETMODE, NG_KD_TEXT);
	}

	if(ioctl(con, NG_KDGKBMODE, &mode) == 0 && mode != NG_K_XLATE && mode != NG_K_UNICODE) {
		log_warn("console: keyboard in raw mode, restoring");
		ioctl(con, NG_KDSKBMODE, NG_K_UNICODE);
	}

	return true;
}

// shutdown messages are read on tty1. a compositor holding the vt releases it on request or
// when it dies, so the switch is asked for twice, the second time with tty1 put back in text mode
void console_front(bool text) {
	int fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);

	if(fd < 0) {
		return;
	}

	if(ioctl(fd, NG_VT_ACTIVATE, 1) < 0) {
		log_warn("console: cannot switch to tty1: %s", strerror(errno));
	}

	close(fd);
	if(!text) {
		return;
	}

	fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if(fd < 0) {
		return;
	}

	console_text(fd);
	close(fd);
}

// whatever died may have left the console unusable
static bool emerg_console_reset(int con) {
	struct termios t;
	bool vt = console_text(con);

	if(tcgetattr(con, &t) == 0) {
		const tcflag_t sane = ISIG | ICANON | ECHO;

		if((t.c_lflag & sane) != sane || !(t.c_oflag & OPOST)) {
			log_warn("console: terminal in raw mode, restoring");
		}

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

#ifdef NINIT_SULOGIN
static bool root_hash_usable(void) {
	FILE* f = fopen("/etc/shadow", "re");
	char line[512];
	bool ok = false;

	if(!f) {
		return false;
	}

	while(fgets(line, sizeof(line), f)) {
		const char* p;

		if(strncmp(line, "root:", 5)) {
			continue;
		}

		p = line + 5;
		ok = *p && *p != ':' && *p != '!' && *p != '*';
		break;
	}

	fclose(f);

	return ok;
}
#endif

static pid_t emerg_spawn(int con, bool vt, int barrier, bool auth) {
	static char sul0[] = "sulogin";
	static char* const sulv[] = { sul0, NULL };
	static char arg0[] = "-sh", path[] = NG_PATH, home[] = "HOME=/";
	static char term_vt[] = "TERM=linux", term_serial[] = "TERM=vt220";
	static char* const argv[] = { arg0, NULL };
	static char* const env_vt[] = { path, term_vt, home, NULL };
	static char* const env_serial[] = { path, term_serial, home, NULL };
	char* const* envp = vt ? env_vt : env_serial;
	sigset_t none;
	pid_t pid;
	int fd, errs[2] = { 0, 0 };

	pid = fork();
	if(pid != 0) {
		return pid;
	}

	// only async-signal-safe calls from here to execve
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	setsid();
	ioctl(con, TIOCSCTTY, 1);
	dup2(con, 0);
	dup2(con, 1);
	dup2(con, 2);
	for(fd = 0; fd < 3; fd++) {
		fcntl(fd, F_SETFD, 0);
	}

	ninit_cloexec_except(-1);

	// oom exemption must not reach whatever is run from this shell
	ninit_oom_score_adj(STR("0\n"));

	// pid 1 may have dropped umask to 0
	(void)!chdir("/");
	umask(022);

	if(auth) {
		execve("/sbin/sulogin", sulv, envp);
		execve("/usr/sbin/sulogin", sulv, envp);
	}

#ifdef NINIT_BUSYBOX
	execve(NINIT_BUSYBOX, argv, envp);
	errs[0] = errno;
#endif
	execve("/bin/sh", argv, envp);
	errs[1] = errno;
	(void)!write(barrier, errs, sizeof(errs));
	_exit(EMERG_NOEXEC);
}

static void emerg_greet(void) {
	if(emerg_greeted) {
		return;
	}

	emerg_greeted = true;
	ninit_log(LOG_DONE, "shell: started on the console, exit with reboot or poweroff");
	// rebuilding the graph does not reload it, pid 1 read it once at boot
	log_note("shell: 'ninitctl resume' retries failed services");
	log_note("shell: 'ninitctl init' takes effect on the next boot");
}

static void emerg_settle(enum emerg_state st, int err) {
	emerg_conf = st;

	if(st == EMERG_EXEC_FAILED) {
		log_err("shell: cannot exec a shell: %s", strerror(err));
		if(++emerg_noexec >= EMERG_NOEXEC_MAX) {
			emerg_gone = true;
			log_err("shell: giving up, no shell can be started");
		}

		return;
	}

	emerg_fail = 0;
	if(st != EMERG_EXEC_OK) {
		return;
	}

	emerg_noexec = 0;
	emerg_greet();
}

static void emerg_bar_close(void) {
	if(emerg_bar >= 0) {
		close(emerg_bar);
	}

	emerg_bar = -1;
	emerg_conf_at = 0;
}

int fail_emergency_fd(void) {
	return emerg_bar;
}

void fail_emergency_report(void) {
	int errs[2] = { 0, 0 };
	ssize_t n;

	if(emerg_bar < 0) {
		return;
	}

	do {
		n = read(emerg_bar, errs, sizeof(errs));
	} while(n < 0 && errno == EINTR);
	if(n < 0) {
		return;
	}

	emerg_bar_close();

	if(n == 0) {
		emerg_settle(EMERG_EXEC_OK, 0);
		return;
	}

	if(n != (ssize_t)sizeof(errs)) {
		log_warn("shell: truncated exec status, state unknown");
		emerg_settle(EMERG_EXEC_UNKNOWN, 0);
		return;
	}

#ifdef NINIT_BUSYBOX
	log_warn("shell: exec %s: %s", NINIT_BUSYBOX, strerror(errs[0]));
#endif
	log_err("shell: exec /bin/sh: %s", strerror(errs[1]));
	emerg_settle(EMERG_EXEC_FAILED, errs[1]);
}

static int emerg_start(void) {
	int bar[2], con, err;
	bool vt, auth = false;
	pid_t pid;

	emerg_pid = -1;

#ifdef NINIT_SULOGIN
	auth = root_hash_usable();
	if(!emerg_greeted) {
		log_warn(auth ? "shell: the root password is required"
			      : "shell: root has no usable password, no authentication");
	}
#endif

	con = emerg_console();
	if(con < 0) {
		log_err("shell: no console available: %s", strerror(errno));
		return -1;
	}

	if(pipe2(bar, O_CLOEXEC)) {
		log_err("shell: pipe: %s", strerror(errno));
		close(con);
		return -1;
	}

	vt = emerg_console_reset(con);
	pid = emerg_spawn(con, vt, bar[1], auth);
	err = errno;
	close(con);
	close(bar[1]);
	if(pid < 0) {
		close(bar[0]);
		log_err("shell: fork: %s", strerror(err));
		return -1;
	}

	clock_gettime(CLOCK_MONOTONIC, &emerg_at);

	emerg_pid = pid;
	emerg_conf = EMERG_EXEC_UNKNOWN;
	fcntl(bar[0], F_SETFL, O_NONBLOCK);
	emerg_bar = bar[0];
	emerg_conf_at = mono_ms() + EMERG_EXEC_MS;

	return 0;
}

static void emerg_restart(void) {
	if(emerg_start() == 0) {
		return;
	}

	if(++emerg_fail >= EMERG_FAIL_MAX) {
		emerg_gone = true;
		log_err("shell: giving up after %" PRIu8 " attempts", emerg_fail);
		return;
	}

	emerg_retry_at = mono_ms() + EMERG_RETRY_MS;
	log_err("shell: not running, retrying in %d ms", EMERG_RETRY_MS);
}

int64_t fail_emergency_due(void) {
	int64_t now = mono_ms(), best = -1;

	if(emerg_conf_at) {
		best = ms_until(emerg_conf_at, now);
	}

	if(emerg_retry_at && !emerg_gone) {
		best = ms_sooner(best, ms_until(emerg_retry_at, now));
	}

	return best;
}

void fail_emergency_tick(void) {
	if(emerg_conf_at && mono_ms() >= emerg_conf_at) {
		emerg_conf_at = 0;
		log_warn("shell: no exec status after %d s, state unknown",
			 EMERG_EXEC_MS / 1000);
		emerg_settle(EMERG_EXEC_UNKNOWN, 0);
	}

	if(!emerg_retry_at || emerg_gone) {
		return;
	}

	if(mono_ms() < emerg_retry_at) {
		return;
	}

	emerg_retry_at = 0;
	if(emerg_pid > 0) {
		return;
	}

	emerg_restart();
}

void fail_emergency_shell(const char* why) {
	log_err("%s", why);

	if(emerg_gone) {
		return;
	}

	if(emerg_pid > 0) {
		log_warn("shell: already running");
		return;
	}

	emerg_restart();
}

bool fail_emergency_reaped(pid_t pid, int status) {
	int64_t ms;
	char how[64];

	if(pid <= 0 || pid != emerg_pid) {
		return false;
	}

	emerg_pid = -1;

	fail_emergency_report();
	emerg_bar_close();
	ms = mono_ms_since(&emerg_at);
	fail_describe(status, how, sizeof(how));

	if(ms >= EMERG_FAST_MS) {
		emerg_fast = 0;
	} else if(++emerg_fast >= EMERG_FAST_MAX) {
		emerg_gone = true;
		log_err("shell: exited immediately %" PRIu8 " times (%s), not restarting",
			emerg_fast, how);
		log_err("shell: giving up, no shell can be started");
		return true;
	}

	if(!(WIFEXITED(status) && WEXITSTATUS(status) == EMERG_NOEXEC)) {
		log_warn("shell: exited (%s)", how);
	}

	if(emerg_gone) {
		return true;
	}

	emerg_restart();

	return true;
}

void fail_summary(const void* map, const uint8_t* state) {
	uint32_t n = ((const struct ng_hdr*)map)->n_svc;
	uint32_t i, failed = 0, skipped = 0;

	for(i = 0; i < n; i++) {
		failed += state[i] == NG_ST_FAILED;
		skipped += state[i] == NG_ST_SKIPPED;
	}

	if(!failed && !skipped) {
		return;
	}

	ninit_log(failed ? LOG_FAIL : LOG_WARN, "boot: %u service%s failed, %u never started",
		  failed, plural(failed), skipped);
	for(i = 0; i < n; i++) {
		if(state[i] == NG_ST_FAILED) {
			log_err("boot: %s failed", ng_name(map, i));
		}
	}

	for(i = 0; i < n; i++) {
		if(state[i] == NG_ST_SKIPPED) {
			log_warn("boot: skipped %s", ng_name(map, i));
		}
	}
}
