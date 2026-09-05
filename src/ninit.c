#include "fail.h"
#include "logging.h"
#include "ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TAIL_CAP	1024
#define LINE_CAP	256
#define STALL_MS	10000
#define START_TIMEOUT_MS 90000
#define TERM_GRACE_MS	5000
#define KILL_GRACE_MS	2000
#define DEGRADED_POLL_MS 200
#define DRAIN_POLL_CHUNKS 16
#define DRAIN_FINAL_CHUNKS 1024
#define SHUTDOWN_DRAIN_MS 100
#define HWCLOCK_GRACE_MS 5000
#define RO_PASSES	3

#define EXIT_NOEXEC	(127 << 8)

#define RESTART_SETTLED_MS 10000

static const unsigned restart_delay_ms[] = { 100, 250, 500, 1000, 2000, 5000 };

struct run {
	pid_t pid;
	int out_fd;
	int ntf_fd;
	uint32_t live_pos;
	long long restart_at;
	uint8_t burst;
	uint8_t attempt;
	uint8_t hup;
	uint8_t timedout;
	uint8_t starting;
	uint16_t tail_len;
	uint16_t line_len;
	long long started;
	char tail[TAIL_CAP];
	char line[LINE_CAP];
};

static const void *map;
static uint32_t n_svc;
static uint8_t *state;
static uint16_t *unmet;
static struct run *runs;
static uint32_t *live, n_live;
static uint32_t *queue, q_head, q_tail;
static int draining;
static uint32_t n_active, n_done;
static int boot_reported, shutting_down;
static int sfd = -1, null_fd = -1;
static struct rlimit child_nofile;
static long long boot_t0;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ensure_stdio(void)
{
	int fd;

	for (fd = 0; fd < 3; fd++) {
		int f;

		if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
			continue;
		f = open("/dev/null", (fd ? O_WRONLY : O_RDONLY) | O_NOCTTY);
		if (f < 0)
			f = open("/", O_RDONLY);
		if (f < 0)
			continue;
		if (f != fd) {
			dup2(f, fd);
			close(f);
		}
	}
}

static int is_mountpoint(const char *path)
{
	struct stat a, b;
	char parent[64];

	if (stat(path, &a) < 0)
		return 0;
	snprintf(parent, sizeof(parent), "%s/..", path);
	if (stat(parent, &b) < 0)
		return 0;
	return a.st_dev != b.st_dev;
}

static void mount_one(const char *src, const char *dst, const char *type,
		      unsigned long flags, const char *data)
{
	if (is_mountpoint(dst))
		return;
	if (mkdir(dst, 0755) < 0 && errno != EEXIST) {
		log_warn("mount: mkdir %s: %s", dst, strerror(errno));
		return;
	}
	if (mount(src, dst, type, flags, data) < 0 && errno != EBUSY)
		log_warn("mount: %s on %s: %s", type, dst, strerror(errno));
}

static void mount_api_fs(void)
{
	mount_one("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount_one("sys", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount_one("dev", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mount_one("run", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
	mount_one("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "mode=0620,gid=5,ptmxmode=0666");
	mount_one("shm", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
}

// devtmpfs makes the device nodes but not these
static void seed_dev(void)
{
	static const char *const link[][2] = {
		{ "/proc/self/fd",	"/dev/fd" },
		{ "/proc/self/fd/0",	"/dev/stdin" },
		{ "/proc/self/fd/1",	"/dev/stdout" },
		{ "/proc/self/fd/2",	"/dev/stderr" },
	};

	for (size_t i = 0; i < sizeof(link) / sizeof(*link); i++)
		if (symlink(link[i][0], link[i][1]) < 0 && errno != EEXIST)
			log_warn("dev: symlink %s: %s", link[i][1], strerror(errno));

	if (access("/proc/kcore", F_OK) == 0 &&
	    symlink("/proc/kcore", "/dev/core") < 0 && errno != EEXIST)
		log_warn("dev: symlink /dev/core: %s", strerror(errno));
}

static void setup_signals(void)
{
	sigset_t all, want;

	sigfillset(&all);
	sigprocmask(SIG_SETMASK, &all, NULL);

	sigemptyset(&want);
	sigaddset(&want, SIGCHLD);
	sigaddset(&want, SIGTERM);
	sigaddset(&want, SIGINT);
	sigaddset(&want, SIGUSR1);
	sigaddset(&want, SIGUSR2);
	sfd = signalfd(-1, &want, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd < 0)
		log_err("signalfd: %s", strerror(errno));

	reboot(RB_DISABLE_CAD);
}

static void raise_nofile(void)
{
	struct rlimit rl;
	rlim_t want = 2 * (rlim_t)NG_MAX_SVC + 256;

	if (getrlimit(RLIMIT_NOFILE, &child_nofile) < 0) {
		child_nofile.rlim_cur = 1024;
		child_nofile.rlim_max = 4096;
	}
	rl = child_nofile;
	if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want)
		rl.rlim_max = want;
	rl.rlim_cur = rl.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &rl) == 0)
		return;
	rl.rlim_max = child_nofile.rlim_max;
	rl.rlim_cur = rl.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &rl) < 0)
		log_warn("setrlimit(RLIMIT_NOFILE): %s", strerror(errno));
}

static const void *load_graph(const char *path, const char **why)
{
	struct stat st;
	void *m;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (fd < 0) {
		*why = strerror(errno);
		return NULL;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
		close(fd);
		*why = "not a regular file";
		return NULL;
	}
	if ((uint64_t)st.st_size > UINT32_MAX) {
		close(fd);
		*why = "larger than 4 GiB";
		return NULL;
	}
	m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		*why = strerror(errno);
		return NULL;
	}
	*why = ng_verify(m, (size_t)st.st_size);
	if (!*why && !((const struct ng_hdr *)m)->n_svc)
		*why = "no services to start";
	if (*why) {
		munmap(m, (size_t)st.st_size);
		return NULL;
	}
	return m;
}

static size_t put_str(char *dst, size_t at, const char *s)
{
	while (*s)
		dst[at++] = *s++;
	return at;
}

static size_t put_num(char *dst, size_t at, unsigned v)
{
	char tmp[10];
	int k = 0;

	do
		tmp[k++] = (char)('0' + v % 10);
	while (v /= 10);
	while (k)
		dst[at++] = tmp[--k];
	return at;
}

static char env_lang[128] = "LANG=" NG_FALLBACK_LANG;

static void load_locale(void)
{
	char lang[96];

	if (ng_locale_lang(lang, sizeof(lang)))
		snprintf(env_lang, sizeof(env_lang), "LANG=%s", lang);
}

static void child_exec(uint32_t i, int out_w, int ntf_w) __attribute__((noreturn));

static void child_exec(uint32_t i, int out_w, int ntf_w)
{
	static char env_path[] = NG_PATH, env_home[] = "HOME=/", env_term[] = "TERM=linux";
	static char *const envp[] = { env_path, env_home, env_term, env_lang, NULL };
	static char arg_bash[] = "bash", arg_sh[] = "sh", arg_c[] = "-c";
	char *argv[5];
	char msg[128];
	struct sigaction dfl = { .sa_handler = SIG_DFL };
	sigset_t none;
	int sig, nfd = ng_notify(map, i), err;
	size_t at;

	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (sig = 1; sig < NSIG; sig++)
		sigaction(sig, &dfl, NULL);

	setsid();
	if (null_fd >= 0)
		dup2(null_fd, 0);
	dup2(out_w, 1);
	dup2(out_w, 2);
	if (nfd) {
		if (ntf_w == nfd)
			fcntl(nfd, F_SETFD, 0);
		else
			dup2(ntf_w, nfd);
	}
	ninit_cloexec_except(nfd ? nfd : -1);
	setrlimit(RLIMIT_NOFILE, &child_nofile);

	// oom_score_adj is inherited, and only pid 1 may be exempt
	ninit_oom_score_adj("0\n", 2);

	(void)!chdir("/");
	umask(022);

	argv[0] = arg_bash;
	argv[1] = arg_c;
	argv[2] = (char *)(uintptr_t)ng_script(map, i);
	argv[3] = (char *)(uintptr_t)ng_name(map, i);
	argv[4] = NULL;
	execve(NG_SHELL, argv, envp);
	err = errno;

	at = put_str(msg, 0, "ninit: exec " NG_SHELL ": errno ");
	at = put_num(msg, at, (unsigned)err);
	at = put_str(msg, at, ", trying /bin/sh\n");
	(void)!write(2, msg, at);

	argv[0] = arg_sh;
	execve("/bin/sh", argv, envp);
	err = errno;
	at = put_str(msg, 0, "ninit: exec /bin/sh: errno ");
	at = put_num(msg, at, (unsigned)err);
	at = put_str(msg, at, "\n");
	(void)!write(2, msg, at);
	_exit(127);
}

static void live_add(uint32_t i)
{
	runs[i].live_pos = n_live;
	live[n_live++] = i;
}

static int live_has(uint32_t i)
{
	return runs[i].live_pos < n_live && live[runs[i].live_pos] == i;
}

static void live_del(uint32_t i)
{
	uint32_t pos = runs[i].live_pos, last = live[--n_live];

	live[pos] = last;
	runs[last].live_pos = pos;
}

static void close_fds(uint32_t i)
{
	struct run *r = &runs[i];

	if (r->out_fd >= 0) {
		close(r->out_fd);
		r->out_fd = -1;
	}
	if (r->ntf_fd >= 0) {
		close(r->ntf_fd);
		r->ntf_fd = -1;
	}
}

static void maybe_free(uint32_t i)
{
	struct run *r = &runs[i];

	if (r->pid == 0 && r->out_fd < 0 && r->ntf_fd < 0 && !r->restart_at && live_has(i))
		live_del(i);
}

static void flush_line(uint32_t i)
{
#ifndef NINIT_QUIET
	struct run *r = &runs[i];

	if (r->line_len)
		log_note("%s: %.*s", ng_name(map, i), (int)r->line_len, r->line);
#endif
	runs[i].line_len = 0;
}

static void absorb(uint32_t i, const char *buf, size_t len)
{
	struct run *r = &runs[i];

	if (len >= TAIL_CAP) {
		memcpy(r->tail, buf + len - TAIL_CAP, TAIL_CAP);
		r->tail_len = TAIL_CAP;
	} else {
		if (r->tail_len + len > TAIL_CAP) {
			size_t keep = TAIL_CAP - len;

			memmove(r->tail, r->tail + r->tail_len - keep, keep);
			r->tail_len = (uint16_t)keep;
		}
		memcpy(r->tail + r->tail_len, buf, len);
		r->tail_len += (uint16_t)len;
	}

#ifndef NINIT_QUIET
	for (size_t k = 0; k < len; k++) {
		if (buf[k] == '\n') {
			flush_line(i);
			continue;
		}
		if (r->line_len == LINE_CAP)
			flush_line(i);
		r->line[r->line_len++] = buf[k];
	}
#endif
}

static void drain_out(uint32_t i, unsigned chunks)
{
	struct run *r = &runs[i];
	char buf[4096];

	while (r->out_fd >= 0 && chunks--) {
		ssize_t k = read(r->out_fd, buf, sizeof(buf));

		if (k > 0) {
			absorb(i, buf, (size_t)k);
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && errno == EAGAIN)
			return;
		close(r->out_fd);
		r->out_fd = -1;
		flush_line(i);
	}
}

static void spawn(uint32_t i);
static void service_failed(uint32_t i, int status);
static void start_failed(uint32_t i, int status);

static void complete(uint32_t i)
{
	runs[i].starting = 0;
	if (state[i] == NG_ST_RUNNING)
		n_active--;
	state[i] = NG_ST_DONE;
	n_done++;
	queue[q_tail++] = i;
	if (draining)
		return;

	draining = 1;
	while (q_head < q_tail) {
		const uint32_t *roff = ng_rdep_off(map), *ridx = ng_rdep_idx(map);
		uint32_t j = queue[q_head++], k;

		for (k = roff[j]; k < roff[j + 1]; k++) {
			uint32_t d = ridx[k];

			if (--unmet[d] != 0 || state[d] != NG_ST_PENDING)
				continue;
			if (ng_svcs(map)[d].type == NG_TYPE_TARGET) {
				log_done("%s", ng_name(map, d));
				complete(d);
			} else {
				spawn(d);
			}
		}
	}
	draining = 0;
	q_head = q_tail = 0;
}

static int launch(uint32_t i)
{
	struct run *r = &runs[i];
	int out[2], ntf[2] = { -1, -1 }, err;
	pid_t pid;

	r->tail_len = 0;
	r->line_len = 0;
	r->hup = 0;
	r->timedout = 0;
	r->starting = 1;

	if (pipe2(out, O_CLOEXEC) < 0) {
		log_err("%s: pipe: %s", ng_name(map, i), strerror(errno));
		return -1;
	}
	fcntl(out[0], F_SETFL, O_NONBLOCK);
	if (ng_notify(map, i)) {
		if (pipe2(ntf, O_CLOEXEC) < 0) {
			log_err("%s: pipe: %s", ng_name(map, i), strerror(errno));
			close(out[0]);
			close(out[1]);
			return -1;
		}
		fcntl(ntf[0], F_SETFL, O_NONBLOCK);
	}

	pid = fork();
	if (pid == 0)
		child_exec(i, out[1], ntf[1]);
	err = errno;
	close(out[1]);
	if (ntf[1] >= 0)
		close(ntf[1]);
	if (pid < 0) {
		close(out[0]);
		if (ntf[0] >= 0)
			close(ntf[0]);
		log_err("%s: fork: %s", ng_name(map, i), strerror(err));
		return -1;
	}

	r->pid = pid;
	r->out_fd = out[0];
	r->ntf_fd = ntf[0];
	r->started = now_ms();

	if (!live_has(i))
		live_add(i);

	return 0;
}

static void spawn(uint32_t i)
{
	if (state[i] != NG_ST_RUNNING) {
		state[i] = NG_ST_RUNNING;
		n_active++;
	}

	if (launch(i) < 0) {
		service_failed(i, EXIT_NOEXEC);
		return;
	}

	if (ng_svcs(map)[i].type == NG_TYPE_DAEMON && !ng_notify(map, i)) {
		log_done("%s", ng_name(map, i));
		complete(i);
	}
}

static void poison(uint32_t i)
{
	uint32_t skipped;

	if (state[i] == NG_ST_RUNNING)
		n_active--;
	skipped = fail_poison(map, i, state);
	if (skipped)
		log_warn("%s: %u dependent service%s will not start", ng_name(map, i),
			 skipped, skipped == 1 ? "" : "s");
}

static void service_failed(uint32_t i, int status)
{
	struct run *r = &runs[i];
	enum fail_act act;
	char why[160];

	drain_out(i, DRAIN_FINAL_CHUNKS);
	close_fds(i);
	if (live_has(i))
		live_del(i);

	if (shutting_down)
		return;

	if (r->attempt < UINT8_MAX)
		r->attempt++;
	act = fail_service(map, i, status, r->attempt, r->tail, r->tail_len);

	switch (act) {
	case FAIL_RETRY:
		spawn(i);
		return;
	case FAIL_SHELL:
		poison(i);
		snprintf(why, sizeof(why), "boot: cannot continue without %s", ng_name(map, i));
		fail_emergency_shell(why);
		return;
	case FAIL_WARN:
	case FAIL_STOP:
	default:
		poison(i);
		return;
	}
}

static void drain_notify(uint32_t i)
{
	struct run *r = &runs[i];
	char buf[256];

	while (r->ntf_fd >= 0) {
		ssize_t k = read(r->ntf_fd, buf, sizeof(buf));

		if (k > 0) {
			if (r->starting && memchr(buf, '\n', (size_t)k)) {
				r->starting = 0;
				log_done("%s (%lld ms)", ng_name(map, i), now_ms() - r->started);
				if (state[i] == NG_ST_RUNNING)
					complete(i);
			}
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && errno == EAGAIN)
			return;

		close(r->ntf_fd);
		r->ntf_fd = -1;
		if (!r->starting) {
			maybe_free(i);
			return;
		}

		if (r->pid > 0) {
			r->hup = 1;
			kill(-r->pid, SIGKILL);
			return;
		}
		start_failed(i, FAIL_ST_NOTIFY_HUP);
		return;
	}
}

static void drain_output(void)
{
	uint32_t k = n_live;

	while (k--) {
		uint32_t i = live[k];

		if (runs[i].out_fd >= 0)
			drain_out(i, DRAIN_POLL_CHUNKS);
	}
}

static void drain_all(void)
{
	uint32_t k = n_live;

	while (k--) {
		uint32_t i = live[k];

		if (runs[i].out_fd >= 0)
			drain_out(i, DRAIN_POLL_CHUNKS);
		if (runs[i].ntf_fd >= 0)
			drain_notify(i);
		if (live_has(i))
			maybe_free(i);
	}
}

static void restart_schedule(uint32_t i, int status)
{
	struct run *r = &runs[i];
	unsigned last = sizeof(restart_delay_ms) / sizeof(*restart_delay_ms) - 1;
	unsigned d;
	char how[64];

	if (now_ms() - r->started >= RESTART_SETTLED_MS)
		r->burst = 0;
	d = restart_delay_ms[r->burst < last ? r->burst : last];
	if (r->burst < last)
		r->burst++;

	close_fds(i);
	r->restart_at = now_ms() + d;

	fail_describe(status, how, sizeof(how));
	log_warn("%s: exited (%s), restarting in %u ms", ng_name(map, i), how, d);
}

static void start_failed(uint32_t i, int status)
{
	runs[i].starting = 0;
	if (state[i] == NG_ST_RUNNING) {
		service_failed(i, status);
		return;
	}
	if (!shutting_down && ng_restart(map, i)) {
		restart_schedule(i, status);
		return;
	}
	close_fds(i);
	maybe_free(i);
}

static void fire_restarts(void)
{
	long long now = now_ms();
	uint32_t k;

	if (shutting_down)
		return;

	for (k = 0; k < n_live; k++) {
		uint32_t i = live[k];
		struct run *r = &runs[i];

		if (!r->restart_at || now < r->restart_at)
			continue;
		r->restart_at = 0;
		r->started = now;
		if (launch(i) < 0)
			restart_schedule(i, EXIT_NOEXEC);
	}
}

static long long restarts_due(void)
{
	long long best = -1, now = now_ms();
	uint32_t k;

	for (k = 0; k < n_live; k++) {
		struct run *r = &runs[live[k]];
		long long d;

		if (!r->restart_at)
			continue;
		d = r->restart_at - now;
		if (d < 0)
			d = 0;
		if (best < 0 || d < best)
			best = d;
	}

	return best;
}

static long long starts_due(void)
{
	long long best = -1, now = now_ms();
	uint32_t k;

	for (k = 0; k < n_live; k++) {
		struct run *r = &runs[live[k]];
		long long d;

		if (!r->starting || r->timedout)
			continue;
		d = r->started + START_TIMEOUT_MS - now;
		if (d < 0)
			d = 0;
		if (best < 0 || d < best)
			best = d;
	}

	return best;
}

static void child_exited(uint32_t i, int status)
{
	struct run *r = &runs[i];
	const char *name = ng_name(map, i);
	char how[64];

	r->pid = 0;
	drain_out(i, DRAIN_FINAL_CHUNKS);

	if (state[i] != NG_ST_RUNNING) {
		r->starting = 0;
		if (state[i] == NG_ST_DONE && !shutting_down) {
			if (r->timedout)
				status = FAIL_ST_TIMEOUT;
			else if (r->hup)
				status = FAIL_ST_NOTIFY_HUP;
			if (ng_restart(map, i)) {
				restart_schedule(i, status);
				return;
			}
			fail_describe(status, how, sizeof(how));
			log_warn("%s: exited (%s)", name, how);
		}
		maybe_free(i);
		return;
	}
	if (shutting_down) {
		close_fds(i);
		maybe_free(i);
		return;
	}
	if (r->timedout) {
		service_failed(i, FAIL_ST_TIMEOUT);
		return;
	}
	if (r->hup) {
		service_failed(i, FAIL_ST_NOTIFY_HUP);
		return;
	}

	if (ng_svcs(map)[i].type == NG_TYPE_ONESHOT) {
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			log_done("%s (%lld ms)", name, now_ms() - r->started);
			complete(i);
			maybe_free(i);
		} else {
			service_failed(i, status);
		}
		return;
	}

	service_failed(i, status);
}

static uint32_t find_pid(pid_t pid)
{
	uint32_t k;

	for (k = 0; k < n_live; k++)
		if (runs[live[k]].pid == pid)
			return live[k];
	return UINT32_MAX;
}

static void reap(void)
{
	for (;;) {
		int status;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		uint32_t i;

		if (pid == 0)
			return;
		if (pid < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		// nothing is restarted once shutdown has begun
		if (shutting_down)
			continue;
		if (fail_emergency_reaped(pid, status))
			continue;
		i = find_pid(pid);
		if (i != UINT32_MAX)
			child_exited(i, status);
	}
}

static void wait_children(long long grace)
{
	long long deadline = now_ms() + grace;

	for (;;) {
		struct pollfd p = { .fd = sfd, .events = POLLIN };
		struct signalfd_siginfo si;
		long long left;

		reap();
		drain_output();
		if (kill(-1, 0) < 0 && errno == ESRCH)
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

static void unescape_mount(char *s)
{
	char *w = s;

	for (; *s; s++) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7' &&
		    s[3] >= '0' && s[3] <= '7') {
			*w++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0'));
			s += 3;
		} else {
			*w++ = *s;
		}
	}
	*w = '\0';
}

static void remount_ro(void)
{
	char *buf = NULL, *p, **mps = NULL;
	size_t cap = 0, len = 0, n = 0, mcap = 0;
	int fd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC | O_NOCTTY);

	if (fd >= 0) {
		for (;;) {
			ssize_t k;

			if (len + 4096 > cap) {
				char *nb = realloc(buf, cap = cap ? cap * 2 : 16384);

				if (!nb)
					break;
				buf = nb;
			}
			k = read(fd, buf + len, cap - len - 1);
			if (k < 0 && errno == EINTR)
				continue;
			if (k <= 0)
				break;
			len += (size_t)k;
		}
		close(fd);
	}

	if (buf) {
		buf[len] = '\0';
		for (p = buf; *p; ) {
			char *nl = strchr(p, '\n'), *sp, *mp;

			if (nl)
				*nl = '\0';
			sp = strchr(p, ' ');
			mp = sp ? sp + 1 : NULL;
			sp = mp ? strchr(mp, ' ') : NULL;
			if (mp && sp) {
				*sp = '\0';
				unescape_mount(mp);
				if (n == mcap) {
					char **nm = realloc(mps, (mcap = mcap ? mcap * 2 : 64) * sizeof(*mps));

					if (!nm)
						break;
					mps = nm;
				}
				mps[n++] = mp;
			}
			if (!nl)
				break;
			p = nl + 1;
		}
	}

	for (int pass = 0; pass < RO_PASSES; pass++) {
		size_t left = 0, k = n;

		while (k--) {
			if (!mps[k])
				continue;
			if (mount(NULL, mps[k], NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0)
				mps[k] = NULL;
			else
				left++;
		}
		if (!left)
			break;
	}

	for (size_t k = n; k--; ) {
		if (!mps[k] || !strcmp(mps[k], "/"))
			continue;
		log_warn("shutdown: %s would not go read-only, detaching it", mps[k]);
		if (umount2(mps[k], MNT_DETACH) < 0)
			log_warn("shutdown: detaching %s: %s", mps[k], strerror(errno));
	}

	for (int pass = 0; pass < RO_PASSES; pass++)
		if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0)
			break;
		else if (pass == RO_PASSES - 1)
			log_err("shutdown: / would not go read-only: %s", strerror(errno));

	free(mps);
	free(buf);
}

// the hardware clock and swap both need the filesystems still writable
static void stop_swap(void)
{
	char line[512];
	FILE *f = fopen("/proc/swaps", "re");

	if (!f)
		return;
	if (!fgets(line, sizeof(line), f)) { // header
		fclose(f);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char *sp = strchr(line, ' ');

		if (!sp)
			sp = strchr(line, '\t');
		if (!sp)
			continue;
		*sp = '\0';
		unescape_mount(line);
		if (swapoff(line) < 0)
			log_warn("shutdown: swapoff %s: %s", line, strerror(errno));
	}
	fclose(f);
}

static int wait_pid_ms(pid_t pid, long long ms)
{
	long long deadline = now_ms() + ms;
	int st;

	for (;;) {
		struct pollfd p = { .fd = sfd, .events = POLLIN };
		struct signalfd_siginfo si;
		pid_t r = waitpid(pid, &st, WNOHANG);
		long long left;

		if (r == pid || (r < 0 && errno != EINTR))
			return 1;
		left = deadline - now_ms();
		if (left <= 0)
			return 0;
		if (left > SHUTDOWN_DRAIN_MS)
			left = SHUTDOWN_DRAIN_MS;
		poll(&p, 1, (int)left);
		while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si))
			;
	}
}

static void save_hwclock(void)
{
	static char arg0[] = "hwclock", arg1[] = "--systohc", arg2[] = "--utc";
	static char *const argv[] = { arg0, arg1, arg2, NULL };
	static char path[] = NG_PATH;
	static char *const envp[] = { path, NULL };
	pid_t pid = fork();

	if (pid < 0)
		return;
	if (pid == 0) {
		ninit_cloexec_except(-1);
		execve("/sbin/hwclock", argv, envp);
		execve("/usr/sbin/hwclock", argv, envp);
		_exit(127);
	}
	if (wait_pid_ms(pid, HWCLOCK_GRACE_MS))
		return;

	log_warn("shutdown: hwclock did not finish in %d s, killing it",
		 HWCLOCK_GRACE_MS / 1000);
	kill(pid, SIGKILL);
	wait_pid_ms(pid, KILL_GRACE_MS);
}

static void shutdown_system(int how, const char *what) __attribute__((noreturn));

static void shutdown_system(int how, const char *what)
{
	shutting_down = 1;
	log_note("%s: sending SIGTERM to everything", what);
	kill(-1, SIGTERM);
	wait_children(TERM_GRACE_MS);
	if (kill(-1, 0) == 0) {
		log_warn("%s: some processes ignored SIGTERM, sending SIGKILL", what);
		kill(-1, SIGKILL);
		wait_children(KILL_GRACE_MS);
	}
	log_note("%s: saving the clock and disabling swap", what);
	save_hwclock();
	stop_swap();

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

static void handle_signals(void)
{
	struct signalfd_siginfo si;

	while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
		switch (si.ssi_signo) {
		case SIGCHLD:
			reap();
			break;
		case SIGTERM:
		case SIGINT:
			shutdown_system(RB_AUTOBOOT, "reboot");
		case SIGUSR1:
			shutdown_system(RB_HALT_SYSTEM, "halt");
		case SIGUSR2:
			shutdown_system(RB_POWER_OFF, "poweroff");
		default:
			break;
		}
	}
}

static void report_stalls(void)
{
	uint32_t k;

	for (k = 0; k < n_live; k++) {
		uint32_t i = live[k];

		if (runs[i].starting)
			log_wait("%s: still running after %lld s", ng_name(map, i),
				 (now_ms() - runs[i].started) / 1000);
	}
}

static void check_timeouts(void)
{
	long long now = now_ms();
	uint32_t k;

	if (shutting_down)
		return;

	for (k = 0; k < n_live; ) {
		uint32_t i = live[k];
		struct run *r = &runs[i];

		if (!r->starting || r->timedout ||
		    now - r->started < START_TIMEOUT_MS) {
			k++;
			continue;
		}

		log_err("%s: no progress after %d s, giving up on it",
			ng_name(map, i), START_TIMEOUT_MS / 1000);
		r->timedout = 1;
		if (r->pid > 0) {
			kill(-r->pid, SIGKILL); // child_exited routes it
			k++;
			continue;
		}

		start_failed(i, FAIL_ST_TIMEOUT);
		k = 0;
	}
}

static void check_boot_done(void)
{
	if (boot_reported || shutting_down || !n_svc || n_active)
		return;
	boot_reported = 1;
	fail_summary(map, state);
	log_note("boot: %u of %u services up in %lld ms", n_done, n_svc, now_ms() - boot_t0);
}

static void start_graph(void)
{
	const struct ng_hdr *h = map;
	uint32_t i;

	n_svc = h->n_svc;
	state = calloc(n_svc, sizeof(*state));
	unmet = calloc(n_svc, sizeof(*unmet));
	runs = calloc(n_svc, sizeof(*runs));
	live = calloc(n_svc, sizeof(*live));
	queue = calloc(n_svc, sizeof(*queue));
	if (!state || !unmet || !runs || !live || !queue) {
		n_svc = 0;
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
			spawn(i);
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

	if (getpid() != 1) {
		log_err("ninit must run as pid 1");
		return 1;
	}

	log_init();
	ensure_stdio();
	placeholder = fcntl(0, F_GETFL) >= 0 && (fcntl(0, F_GETFL) & O_ACCMODE) == O_RDONLY &&
		      fcntl(1, F_GETFL) >= 0 && (fcntl(1, F_GETFL) & O_ACCMODE) == O_RDONLY;
	umask(022);
	(void)!chdir("/");
	setup_signals();
	mount_api_fs();
	seed_dev();

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

	pfds = calloc(1 + 2 * (size_t)n_svc, sizeof(*pfds));
	pf_idx = calloc(1 + 2 * (size_t)n_svc, sizeof(*pf_idx));
	pf_kind = calloc(1 + 2 * (size_t)n_svc, sizeof(*pf_kind));
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

	for (;;) {
		static long long last_stall;
		long long wait, due;
		nfds_t nfds = 1, k;
		int rc;

		fire_restarts();
		check_timeouts();
		check_boot_done();

		if (degraded)
			drain_all();

		pfds[0].fd = sfd;
		pfds[0].events = POLLIN;
		for (k = 0; !degraded && k < n_live; k++) {
			uint32_t i = live[k];

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
				pf_kind[nfds++] = 1;
			}
		}

		wait = n_active && !shutting_down ? STALL_MS : -1;
		due = restarts_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		due = starts_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		if (degraded && (wait < 0 || wait > DEGRADED_POLL_MS))
			wait = DEGRADED_POLL_MS;

		rc = poll(pfds, nfds, (int)wait);
		if (rc < 0) {
			if (errno != EINTR) {
				log_err("poll: %s", strerror(errno));
				poll(NULL, 0, 1000);
			}
			continue;
		}
		if (rc == 0) {
			if (now_ms() - last_stall >= STALL_MS) {
				last_stall = now_ms();
				report_stalls();
			}
			continue;
		}

		for (k = 1; k < nfds; k++) {
			uint32_t i = pf_idx[k];

			if (!pfds[k].revents)
				continue;
			if (pf_kind[k])
				drain_notify(i);
			else
				drain_out(i, DRAIN_POLL_CHUNKS);
			if (live_has(i))
				maybe_free(i);
		}
		if (pfds[0].revents)
			handle_signals();
	}
}
