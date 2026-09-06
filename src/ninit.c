#include "fail.h"
#include "logging.h"
#include "nctl.h"
#include "ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
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
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/un.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TAIL_CAP	1024
#define LINE_CAP	256
#define STALL_MS	10000
#define TERM_GRACE_MS	5000
#define KILL_GRACE_MS	2000
#define DEGRADED_POLL_MS 200
#define DRAIN_POLL_CHUNKS 16
#define DRAIN_BUDGET	262144u
#define DRAIN_FINAL_CHUNKS 64
#define SHUTDOWN_DRAIN_MS 100
#define HWCLOCK_GRACE_MS 5000
#define SIGNAL_POLL_MS	200
#define RO_PASSES	3
#define STOP_TOTAL_MS	30000
#define PF_KTHREAD	0x00200000ul
#define CG_BASE		"/sys/fs/cgroup"
#define CG_DIR		CG_BASE "/ninit.services"
#define CG_MAGIC	0x63677270
#define CG_PATH_MAX	(sizeof(CG_DIR) + NG_MAX_NAME + 24)
#define CTL_MAX		8
#define CTL_BUF		NCTL_REQ_MAX
#define CTL_OUT		8192
#define CTL_ACK_MS	30000

#define EXIT_NOEXEC	(127 << 8)

#define SVC_OP_NONE	0
#define SVC_OP_TERM	1
#define SVC_OP_KILL	2
#define SVC_OP_START	3

#define RESTART_SETTLED_MS 10000

static const unsigned restart_delay_ms[] = { 100, 250, 500, 1000, 2000, 5000 };

struct run {
	pid_t pid;
	pid_t pgid;
	pid_t stale_pid;
	int out_fd;
	int ntf_fd;
	uint32_t live_pos;
	long long restart_at;
	uint8_t burst;

	uint8_t attempt;
	uint8_t hup;
	uint8_t timedout;
	uint8_t starting;
	uint8_t ntf_exec;
	uint8_t op;
	uint8_t op_restart;
	uint16_t tail_len;
	uint16_t line_len;
	long long started;
	long long op_at;
	char tail[TAIL_CAP];
	char line[LINE_CAP];
};

static const void *map;
static uint32_t n_svc;
static uint8_t *state;
static uint8_t *up;
static uint8_t *want;
static uint16_t *unmet;
static struct run *runs;
static uint32_t *live, n_live;
static uint32_t *queue, q_head, q_tail;
static uint32_t *rqueue;
static uint8_t *released;
static int draining;
static uint32_t n_active, n_done, n_pending, n_up;
static uint32_t drain_left;
static uint32_t drain_rotor;
static int boot_reported, shutting_down;

struct ctl {
	int fd;
	uint32_t svc;
	uint32_t list_at;
	uint8_t listing;
	uint8_t done;
	uint16_t in_len;
	uint16_t out_at, out_len;
	char in[CTL_BUF];
	char out[CTL_OUT];
};

static int ctl_lfd = -1;
static uint32_t n_ops;
static struct ctl ctl_conn[CTL_MAX];
static int n_ctl;
static int sfd = -1, null_fd = -1;
static sigset_t sig_want;
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

static int cg_ok;

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
	mount_one("cgroup2", CG_BASE, "cgroup2", MS_NOSUID | MS_NOEXEC | MS_NODEV, "nsdelegate");
}

static void cgroup_init(void)
{
	struct statfs sf;

	if (statfs(CG_BASE, &sf) < 0 || (unsigned long)sf.f_type != CG_MAGIC) {
		log_warn("cgroup: %s is not cgroup2, services are contained by process group only",
			 CG_BASE);
		return;
	}
	if (mkdir(CG_DIR, 0755) < 0 && errno != EEXIST) {
		log_warn("cgroup: mkdir %s: %s", CG_DIR, strerror(errno));
		return;
	}
	if (mkdir(CG_DIR "/.probe", 0755) < 0 && errno != EEXIST) {
		log_warn("cgroup: cannot create %s/*: %s, services are contained by process group only",
			 CG_DIR, strerror(errno));
		return;
	}
	rmdir(CG_DIR "/.probe");
	cg_ok = 1;
}

static int cg_path(uint32_t i, const char *leaf, char *buf, size_t cap)
{
	int n = snprintf(buf, cap, CG_DIR "/%s%s%s", ng_name(map, i), *leaf ? "/" : "", leaf);

	return n > 0 && (size_t)n < cap;
}

static void cgroup_make(uint32_t i, char *procs, size_t cap)
{
	char dir[CG_PATH_MAX];

	procs[0] = '\0';
	if (!cg_ok)
		return;
	if (!cg_path(i, "", dir, sizeof(dir)) ||
	    !cg_path(i, "cgroup.procs", procs, cap)) {
		procs[0] = '\0';
		log_warn("%s: cgroup path is too long, falling back to its process group",
			 ng_name(map, i));
		return;
	}
	if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
		procs[0] = '\0';
		log_warn("%s: mkdir %s: %s, falling back to its process group",
			 ng_name(map, i), dir, strerror(errno));
	}
}

static int cgroup_populated(uint32_t i)
{
	char path[CG_PATH_MAX], buf[256], *p;
	ssize_t k;
	int fd;

	if (!cg_ok || !cg_path(i, "cgroup.events", path, sizeof(path)))
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	k = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (k <= 0)
		return -1;
	buf[k] = '\0';
	p = strstr(buf, "populated");
	if (!p)
		return -1;
	p += 9;
	while (*p == ' ' || *p == '\t')
		p++;
	return *p == '1';
}

static void cgroup_drop(uint32_t i)
{
	char dir[CG_PATH_MAX];

	if (!cg_ok || !cg_path(i, "", dir, sizeof(dir)))
		return;
	if (rmdir(dir) == 0 || errno == ENOENT)
		return;
	log_warn("%s: rmdir %s: %s", ng_name(map, i), dir, strerror(errno));
}

static int cgroup_signal(uint32_t i, int sig)
{
	char path[CG_PATH_MAX], buf[4096];
	size_t held = 0;
	ssize_t k;
	int fd, ok;

	if (!cg_ok)
		return -1;

	if (sig == SIGKILL) {
		if (!cg_path(i, "cgroup.kill", path, sizeof(path)))
			return -1;
		fd = open(path, O_WRONLY | O_CLOEXEC);
		if (fd < 0)
			return -1;
		ok = write(fd, "1", 1) == 1;
		close(fd);
		return ok ? 0 : -1;
	}

	if (!cg_path(i, "cgroup.procs", path, sizeof(path)))
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	while ((k = read(fd, buf + held, sizeof(buf) - held - 1)) > 0) {
		char *p = buf, *nl;

		buf[held + (size_t)k] = '\0';
		while ((nl = strchr(p, '\n')) != NULL) {
			long pid;

			*nl = '\0';
			pid = strtol(p, NULL, 10);
			if (pid > 1)
				kill((pid_t)pid, sig);
			p = nl + 1;
		}
		held = strlen(p);
		if (held >= sizeof(buf) - 1)
			held = 0;
		else
			memmove(buf, p, held);
	}
	close(fd);
	return 0;
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
	sigset_t all;

	sigfillset(&all);
	sigprocmask(SIG_SETMASK, &all, NULL);

	sigemptyset(&sig_want);
	sigaddset(&sig_want, SIGCHLD);
	sigaddset(&sig_want, SIGTERM);
	sigaddset(&sig_want, SIGINT);
	sigaddset(&sig_want, SIGUSR1);
	sigaddset(&sig_want, SIGUSR2);
	sfd = signalfd(-1, &sig_want, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd < 0)
		log_err("signalfd: %s, dequeuing signals directly instead", strerror(errno));

	reboot(RB_DISABLE_CAD);
}

static void raise_nofile(void)
{
	struct rlimit rl;
	rlim_t need = 2 * (rlim_t)NG_MAX_SVC + 256;

	if (getrlimit(RLIMIT_NOFILE, &child_nofile) < 0) {
		child_nofile.rlim_cur = 1024;
		child_nofile.rlim_max = 4096;
	}
	rl = child_nofile;
	if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < need)
		rl.rlim_max = need;
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

	m = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) {
		close(fd);
		*why = strerror(errno);
		return NULL;
	}

	for (off_t got = 0; got < st.st_size; ) {
		ssize_t k = read(fd, (char *)m + got, (size_t)(st.st_size - got));

		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0) {
			close(fd);
			munmap(m, (size_t)st.st_size);
			*why = k < 0 ? strerror(errno) : "shorter than it claimed";
			return NULL;
		}
		got += k;
	}
	close(fd);
	if (mprotect(m, (size_t)st.st_size, PROT_READ) < 0) {
		munmap(m, (size_t)st.st_size);
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

static char env_locale[NG_LOCALE_MAX][NG_LOCALE_LEN] = { "LANG=" NG_FALLBACK_LANG };
static int n_locale = 1;

static void load_locale(void)
{
	const char *why;
	int n = ng_locale_env(env_locale, NG_LOCALE_MAX, &why);

	if (why)
		log_warn("locale: %s %s, using what it could parse", NG_LOCALE_CONF, why);
	if (n > 0) {
		n_locale = n;
		return;
	}
	snprintf(env_locale[0], NG_LOCALE_LEN, "LANG=%s", NG_FALLBACK_LANG);
	n_locale = 1;
}

static void child_exec(uint32_t i, int out_w, int ntf_w, const char *cg) __attribute__((noreturn));

static void child_exec(uint32_t i, int out_w, int ntf_w, const char *cg)
{
	static char env_path[] = NG_PATH, env_home[] = "HOME=/", env_term[] = "TERM=linux";
	static char *envp[4 + NG_LOCALE_MAX];
	static char arg_shell[] = NG_SHELL_ARGV0, arg_c[] = "-c";
	char *argv[5];
	int e = 0;
	char msg[128];
	struct sigaction dfl = { .sa_handler = SIG_DFL };
	sigset_t none;
	int sig, nfd = ng_notify(map, i), err;
	size_t at;

	envp[e++] = env_path;
	envp[e++] = env_home;
	envp[e++] = env_term;
	for (int k = 0; k < n_locale; k++)
		envp[e++] = env_locale[k];
	envp[e] = NULL;

	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (sig = 1; sig < NSIG; sig++)
		sigaction(sig, &dfl, NULL);

	setsid();
	if (*cg) {
		int cf = open(cg, O_WRONLY | O_CLOEXEC);
		int joined = 0;

		if (cf >= 0) {
			joined = write(cf, "0", 1) == 1;
			close(cf);
		}
		if (!joined) {
			// out_w is already this service's stdout so pid 1 logs it
			at = put_str(msg, 0, "ninit: could not join its cgroup: errno ");
			at = put_num(msg, at, (unsigned)errno);
			at = put_str(msg, at, ", it is only contained by its process group\n");
			(void)!write(out_w, msg, at);
		}
	}
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

	argv[0] = arg_shell;
	argv[1] = arg_c;
	argv[2] = (char *)(uintptr_t)ng_script(map, i);
	argv[3] = (char *)(uintptr_t)ng_name(map, i);
	argv[4] = NULL;
	execve(NG_SHELL, argv, envp);
	err = errno;

	at = put_str(msg, 0, "ninit: exec " NG_SHELL ": errno ");
	at = put_num(msg, at, (unsigned)err);
	at = put_str(msg, at, "\n");
	(void)!write(2, msg, at);
	if (ntf_w >= 0 && !nfd)
		(void)!write(ntf_w, "x", 1);
	_exit(127);
}

static pid_t *pid_key;
static uint32_t *pid_val, pid_mask;

static uint32_t pid_hash(pid_t p)
{
	return (uint32_t)((uint32_t)p * 0x9e3779b1u) & pid_mask;
}

static void pid_put(pid_t pid, uint32_t svc)
{
	uint32_t i;

	if (!pid_key)
		return;
	for (i = pid_hash(pid); pid_key[i]; i = (i + 1) & pid_mask)
		if (pid_key[i] == pid)
			break;
	pid_key[i] = pid;
	pid_val[i] = svc;
}

static void pid_del(pid_t pid)
{
	uint32_t i, j, k;

	if (!pid_key)
		return;
	for (i = pid_hash(pid); pid_key[i]; i = (i + 1) & pid_mask)
		if (pid_key[i] == pid)
			break;
	if (!pid_key[i])
		return;

	for (j = i;;) {
		pid_key[i] = 0;
		for (;;) {
			j = (j + 1) & pid_mask;
			if (!pid_key[j])
				return;
			k = pid_hash(pid_key[j]);
			if (i <= j ? (k <= i || k > j) : (k <= i && k > j))
				break;
		}
		pid_key[i] = pid_key[j];
		pid_val[i] = pid_val[j];
		i = j;
	}
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

static void signal_group(uint32_t i, int sig)
{
	cgroup_signal(i, sig);
	if (runs[i].pgid > 0)
		kill(-runs[i].pgid, sig);
}

static void kill_group(uint32_t i)
{
	signal_group(i, SIGKILL);
	runs[i].pgid = 0;
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

	if (r->pid == 0 && !r->stale_pid && r->out_fd < 0 && r->ntf_fd < 0 &&
	    !r->restart_at && r->op == SVC_OP_NONE && live_has(i)) {
		cgroup_drop(i);
		live_del(i);
	}
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

static void drain_out(uint32_t i, unsigned chunks, int budgeted)
{
	struct run *r = &runs[i];
	char buf[4096];

	while (r->out_fd >= 0 && chunks--) {
		size_t take = sizeof(buf);
		ssize_t k;

		if (budgeted) {
			if (!drain_left) {
				drain_rotor = i;
				return;
			}
			if (drain_left < take)
				take = drain_left;
		}
		k = read(r->out_fd, buf, take);

		if (k > 0) {
			if (budgeted)
				drain_left -= (uint32_t)k;
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

static void svc_abandon(uint32_t i)
{
	struct run *r = &runs[i];

	if (r->pid <= 0)
		return;
	r->stale_pid = r->pid;
	r->pid = 0;
}

static int svc_gone(uint32_t i)
{
	const struct run *r = &runs[i];

	return r->pid <= 0 && !r->stale_pid && cgroup_populated(i) <= 0;
}

static int svc_can_start(uint32_t i, const char **why)
{
	const struct run *r = &runs[i];

	if (shutting_down) {
		*why = "shutting down";
		return 0;
	}
	if (want[i]) {
		*why = "held stopped";
		return 0;
	}
	if (r->pid > 0) {
		*why = "already running";
		return 0;
	}
	if (r->stale_pid) {
		*why = "its previous instance has not exited";
		return 0;
	}
	if (unmet[i]) {
		*why = "waiting on a prerequisite";
		return 0;
	}
	return 1;
}

static int svc_try_start(uint32_t i)
{
	const char *why;

	if (!svc_can_start(i, &why))
		return 0;
	spawn(i);
	return 1;
}

static void svc_mark_done(uint32_t i)
{
	runs[i].starting = 0;
	if (state[i] == NG_ST_RUNNING)
		n_active--;
	else if (state[i] == NG_ST_PENDING)
		n_pending--;
	if (state[i] != NG_ST_DONE) {
		state[i] = NG_ST_DONE;
		n_done++;
	}
}

static void svc_set_up(uint32_t i, int v)
{
	if (!up[i] == !v)
		return;
	up[i] = (uint8_t)!!v;
	if (v)
		n_up++;
	else
		n_up--;
}

static void svc_release(uint32_t i)
{
	const uint32_t *roff = ng_rdep_off(map), *ridx = ng_rdep_idx(map);

	if (released[i])
		return;
	released[i] = 1;
	queue[q_tail++] = i;
	if (draining)
		return;

	draining = 1;
	while (q_head < q_tail) {
		uint32_t j = queue[q_head++], k;

		for (k = roff[j]; k < roff[j + 1]; k++) {
			uint32_t d = ridx[k];

			if (--unmet[d] != 0 || want[d])
				continue;
			if (ng_svcs(map)[d].type == NG_TYPE_TARGET) {
				if (released[d])
					continue;
				svc_set_up(d, 1);
				svc_mark_done(d);
				log_done("%s", ng_name(map, d));
				released[d] = 1;
				queue[q_tail++] = d;
				continue;
			}
			if (state[d] == NG_ST_PENDING) {
				svc_try_start(d);
			} else if (state[d] == NG_ST_DONE && !up[d] && ng_restart(map, d)) {
				// spawn() would reset it to a fresh startup and
				// start-tries could then retire a supervised daemon
				if (!live_has(d))
					live_add(d);
				if (!runs[d].restart_at)
					runs[d].restart_at = now_ms();
			}
		}
	}
	draining = 0;
	q_head = q_tail = 0;
}

static void svc_reclaim(uint32_t i)
{
	const uint32_t *roff = ng_rdep_off(map), *ridx = ng_rdep_idx(map);
	uint32_t rh = 0, rt = 0;

	if (!released[i] || ng_order_only(map, i))
		return;
	released[i] = 0;
	rqueue[rt++] = i;

	while (rh < rt) {
		uint32_t j = rqueue[rh++], k;

		for (k = roff[j]; k < roff[j + 1]; k++) {
			uint32_t d = ridx[k];

			unmet[d]++;
			if (ng_svcs(map)[d].type != NG_TYPE_TARGET)
				continue;
			if (!released[d] || ng_order_only(map, d))
				continue;
			released[d] = 0;
			svc_set_up(d, 0);
			rqueue[rt++] = d;
		}
	}
}

static void go_down(uint32_t i)
{
	if (!up[i] && !released[i])
		return;
	svc_set_up(i, 0);
	svc_reclaim(i);
}

static void complete(uint32_t i)
{
	svc_mark_done(i);
	svc_set_up(i, 1);
	svc_release(i);
}

static int launch(uint32_t i)
{
	struct run *r = &runs[i];
	int out[2], ntf[2] = { -1, -1 }, err;
	char cg[CG_PATH_MAX];
	pid_t pid;

	r->tail_len = 0;
	r->line_len = 0;
	r->hup = 0;
	r->timedout = 0;
	r->starting = 1;
	// a daemon that reports nothing is at least held until the shell execs
	r->ntf_exec = ng_svcs(map)[i].type == NG_TYPE_DAEMON && !ng_notify(map, i);

	if (pipe2(out, O_CLOEXEC) < 0) {
		log_err("%s: pipe: %s", ng_name(map, i), strerror(errno));
		return -1;
	}
	fcntl(out[0], F_SETFL, O_NONBLOCK);
	if (ng_notify(map, i) || r->ntf_exec) {
		if (pipe2(ntf, O_CLOEXEC) < 0) {
			log_err("%s: pipe: %s", ng_name(map, i), strerror(errno));
			close(out[0]);
			close(out[1]);
			return -1;
		}
		fcntl(ntf[0], F_SETFL, O_NONBLOCK);
	}

	cgroup_make(i, cg, sizeof(cg));

	pid = fork();
	if (pid == 0)
		child_exec(i, out[1], ntf[1], cg);
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
	r->pgid = pid;
	pid_put(pid, i);
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
		if (state[i] == NG_ST_PENDING)
			n_pending--;
		state[i] = NG_ST_RUNNING;
		n_active++;
	}

	if (launch(i) < 0)
		service_failed(i, EXIT_NOEXEC);
}

static void poison(uint32_t i)
{
	uint32_t skipped, undone;

	if (state[i] == NG_ST_RUNNING)
		n_active--;
	else if (state[i] == NG_ST_PENDING)
		n_pending--;
	go_down(i);
	skipped = fail_poison(map, i, state, up, &undone);
	n_pending -= skipped;
	n_done -= undone;
	if (skipped)
		log_warn("%s: %u dependent service%s will not start", ng_name(map, i),
			 skipped, skipped == 1 ? "" : "s");
}

// a service that had completed is gone for good
static void poison_deps(uint32_t i)
{
	uint32_t skipped, undone;

	if (ng_order_only(map, i))
		return;
	skipped = fail_poison_deps(map, i, state, up, &undone);

	n_pending -= skipped;
	n_done -= undone;
	if (skipped)
		log_warn("%s: %u dependent service%s will not start", ng_name(map, i),
			 skipped, skipped == 1 ? "" : "s");
}

static void service_failed(uint32_t i, int status)
{
	struct run *r = &runs[i];
	enum fail_act act;
	char why[160];

	kill_group(i);
	drain_out(i, DRAIN_FINAL_CHUNKS, 0);
	close_fds(i);
	r->starting = 0;

	svc_abandon(i);
	if (live_has(i) && !r->stale_pid && r->op == SVC_OP_NONE)
		live_del(i);

	if (shutting_down)
		return;

	if (r->attempt < UINT8_MAX)
		r->attempt++;
	act = fail_service(map, i, status, r->attempt, ng_start_tries(map, i),
			   r->tail, r->tail_len);

	switch (act) {
	case FAIL_RETRY: {
		unsigned d = ng_pol(map, i)->retry_ms;

		if (!d && svc_try_start(i))
			return;
		if (!live_has(i))
			live_add(i);
		r->restart_at = now_ms() + (r->stale_pid ? KILL_GRACE_MS : d);
		return;
	}
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

static void give_up(uint32_t i, int status)
{
	struct run *r = &runs[i];
	unsigned tries = ng_start_tries(map, i);
	char why[160];

	if (fail_service(map, i, status, tries, tries, r->tail, r->tail_len) == FAIL_SHELL) {
		poison(i);
		snprintf(why, sizeof(why), "boot: cannot continue without %s", ng_name(map, i));
		fail_emergency_shell(why);
		return;
	}
	poison(i);
}

static void drain_notify(uint32_t i, unsigned chunks)
{
	struct run *r = &runs[i];
	char buf[256];

	while (r->ntf_fd >= 0 && chunks--) {
		ssize_t k = read(r->ntf_fd, buf, sizeof(buf));

		if (k > 0) {
			if (r->starting && !r->timedout && !r->hup && memchr(buf, '\n', (size_t)k)) {
				r->starting = 0;
				log_done("%s (%lld ms)", ng_name(map, i), now_ms() - r->started);
				if (state[i] == NG_ST_RUNNING || state[i] == NG_ST_DONE)
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
			signal_group(i, SIGKILL);
			return;
		}
		start_failed(i, FAIL_ST_NOTIFY_HUP);
		return;
	}
}

static void drain_exec(uint32_t i)
{
	struct run *r = &runs[i];
	char buf[8];

	while (r->ntf_fd >= 0) {
		ssize_t k = read(r->ntf_fd, buf, sizeof(buf));

		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && errno == EAGAIN)
			return;

		close(r->ntf_fd);
		r->ntf_fd = -1;
		if (k > 0 || !r->starting)
			return;
		r->starting = 0;
		log_done("%s (%lld ms)", ng_name(map, i), now_ms() - r->started);
		if (state[i] == NG_ST_RUNNING || state[i] == NG_ST_DONE)
			complete(i);
	}
}

static void drain_output(void)
{
	uint32_t k = n_live;

	while (k--) {
		uint32_t i = live[k];

		if (runs[i].out_fd >= 0)
			drain_out(i, DRAIN_POLL_CHUNKS, 1);
	}
}

static void drain_all(void)
{
	uint32_t k = n_live;

	while (k--) {
		uint32_t i = live[k];

		if (runs[i].out_fd >= 0)
			drain_out(i, DRAIN_POLL_CHUNKS, 1);
		if (runs[i].ntf_fd >= 0) {
			if (runs[i].ntf_exec)
				drain_exec(i);
			else
				drain_notify(i, DRAIN_POLL_CHUNKS);
		}
		if (live_has(i))
			maybe_free(i);
	}
}

static void restart_schedule(uint32_t i, int status)
{
	struct run *r = &runs[i];
	unsigned last = sizeof(restart_delay_ms) / sizeof(*restart_delay_ms) - 1;
	long long alive = now_ms() - r->started;
	unsigned d;
	int first;
	char how[64];

	if (alive >= RESTART_SETTLED_MS)
		r->burst = 0;
	first = r->burst == 0;
	d = restart_delay_ms[r->burst < last ? r->burst : last];
	if (r->burst < last)
		r->burst++;

	kill_group(i);
	drain_out(i, DRAIN_FINAL_CHUNKS, 0);
	close_fds(i);
	svc_abandon(i);
	r->restart_at = now_ms() + d;

	fail_describe(status, how, sizeof(how));
	log_warn("%s: exited (%s) after %lld ms up, restarting in %u ms",
		 ng_name(map, i), how, alive, d);

	if (first && r->tail_len)
		log_raw(LOG_WARN, r->tail, r->tail_len);
}

static void start_failed(uint32_t i, int status)
{
	runs[i].starting = 0;
	if (state[i] == NG_ST_RUNNING) {
		service_failed(i, status);
		return;
	}
	if (!shutting_down)
		go_down(i);
	if (!shutting_down && ng_restart(map, i)) {
		restart_schedule(i, status);
		return;
	}
	close_fds(i);
	if (!shutting_down)
		poison_deps(i);
	maybe_free(i);
}

static void fire_restarts(void)
{
	long long now = now_ms();
	uint32_t k;

	if (shutting_down)
		return;

	// descending: giving up on a service swaps the last one into its slot,
	// and that one has already been seen
	k = n_live;
	while (k--) {
		uint32_t i = live[k];
		struct run *r = &runs[i];
		const char *why;

		if (!r->restart_at || now < r->restart_at)
			continue;
		r->restart_at = 0;
		if (want[i])
			continue;

		if (!svc_can_start(i, &why)) {
			if (r->stale_pid && state[i] == NG_ST_RUNNING) {
				log_err("%s: the killed instance (pid %d) has not exited, not starting another",
					ng_name(map, i), (int)r->stale_pid);
				give_up(i, FAIL_ST_TIMEOUT);
				continue;
			}
			if (r->stale_pid || r->pid > 0 || state[i] == NG_ST_RUNNING)
				r->restart_at = now + KILL_GRACE_MS;
			continue;
		}

		r->started = now;
		if (launch(i) < 0) {
			if (state[i] == NG_ST_RUNNING)
				service_failed(i, EXIT_NOEXEC);
			else
				restart_schedule(i, EXIT_NOEXEC);
		}
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
		d = r->started + ng_start_ms(map, live[k]) - now;
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

	drain_out(i, DRAIN_FINAL_CHUNKS, 0);

	if (r->ntf_fd >= 0 && !r->ntf_exec)
		drain_notify(i, DRAIN_FINAL_CHUNKS);
	pid_del(r->pid);
	r->pid = 0;

	if (want[i] && !shutting_down) {
		r->starting = 0;
		r->restart_at = 0;
		go_down(i);
		kill_group(i);
		drain_out(i, DRAIN_FINAL_CHUNKS, 0);
		close_fds(i);
		if (state[i] == NG_ST_RUNNING) {
			n_active--;
			state[i] = NG_ST_PENDING;
			n_pending++;
		}
		maybe_free(i);
		return;
	}
	if (state[i] != NG_ST_RUNNING) {
		r->starting = 0;
		if (state[i] == NG_ST_DONE && !shutting_down) {
			if (r->timedout)
				status = FAIL_ST_TIMEOUT;
			else if (r->hup)
				status = FAIL_ST_NOTIFY_HUP;
			go_down(i);
			if (ng_restart(map, i)) {
				restart_schedule(i, status);
				return;
			}
			fail_describe(status, how, sizeof(how));
			log_warn("%s: exited (%s) after %lld ms up", name, how,
				 now_ms() - r->started);
			kill_group(i);
			drain_out(i, DRAIN_FINAL_CHUNKS, 0);
			if (r->tail_len)
				log_raw(LOG_WARN, r->tail, r->tail_len);
			close_fds(i);
			poison_deps(i);
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

static uint32_t find_pid(pid_t pid, int *stale)
{
	uint32_t i, k;

	if (pid_key) {
		for (i = pid_hash(pid); pid_key[i]; i = (i + 1) & pid_mask)
			if (pid_key[i] == pid) {
				*stale = runs[pid_val[i]].pid != pid;
				return pid_val[i];
			}
		return UINT32_MAX;
	}

	for (k = 0; k < n_live; k++) {
		uint32_t j = live[k];

		if (runs[j].pid == pid) {
			*stale = 0;
			return j;
		}
		if (runs[j].stale_pid == pid) {
			*stale = 1;
			return j;
		}
	}
	return UINT32_MAX;
}

// an abandoned instance finally died
static void stale_reaped(uint32_t i, pid_t pid)
{
	struct run *r = &runs[i];

	pid_del(pid);
	if (r->stale_pid == pid)
		r->stale_pid = 0;
	if (r->restart_at)
		r->restart_at = now_ms() + ng_pol(map, i)->retry_ms;
	maybe_free(i);
}

static void reap(void)
{
	for (;;) {
		int status, stale;
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
		if (!shutting_down && fail_emergency_reaped(pid, status))
			continue;
		i = find_pid(pid, &stale);
		if (i == UINT32_MAX)
			continue;
		if (stale)
			stale_reaped(i, pid);
		else
			child_exited(i, status);
	}
}

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

	size_t stuck = 0;

	for (size_t k = n; k--; ) {
		if (!mps[k] || !strcmp(mps[k], "/"))
			continue;
		if (umount2(mps[k], 0) == 0) {
			log_warn("shutdown: %s would not go read-only, unmounted it", mps[k]);
			mps[k] = NULL;
			continue;
		}
		stuck++;
	}

	if (stuck) {
		sync();
		for (size_t k = n; k--; ) {
			if (!mps[k] || !strcmp(mps[k], "/"))
				continue;
			if (mount(NULL, mps[k], NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0) {
				log_warn("shutdown: %s went read-only on the second try", mps[k]);
				mps[k] = NULL;
				continue;
			}
			if (umount2(mps[k], 0) == 0) {
				log_warn("shutdown: %s unmounted on the second try", mps[k]);
				mps[k] = NULL;
				continue;
			}
			log_err("shutdown: %s is still mounted writable (%s); detaching it, "
				"its data may not be flushed", mps[k], strerror(errno));
			if (umount2(mps[k], MNT_DETACH) < 0)
				log_warn("shutdown: detaching %s: %s", mps[k], strerror(errno));
		}
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

	if (!n_svc || build_prereqs(&off, &idx) < 0)
		goto out;
	waitc = calloc(n_svc, sizeof(*waitc));
	stop_at = calloc(n_svc, sizeof(*stop_at));
	st = calloc(n_svc, sizeof(*st));
	if (!waitc || !stop_at || !st)
		goto out;

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
			log_warn("shutdown: %u service%s would not stop in order", stuck,
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

static void shutdown_system(int how, const char *what) __attribute__((noreturn));

static void shutdown_system(int how, const char *what)
{
	shutting_down = 1;
	stop_ordered();
	log_note("%s: sending SIGTERM to everything", what);
	kill(-1, SIGTERM);
	wait_children(TERM_GRACE_MS);
	if (userspace_left()) {
		log_warn("%s: some processes ignored SIGTERM, sending SIGKILL", what);
		kill(-1, SIGKILL);
		wait_children(KILL_GRACE_MS);
	}
	log_note("%s: saving the clock and disabling swap", what);
	save_hwclock();
	stop_swap();

	if (ctl_lfd >= 0)
		unlink(NINIT_CTL_SOCK);
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

static void dispatch_signal(int signo)
{
	switch (signo) {
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

static void handle_signals(void)
{
	struct signalfd_siginfo si;

	while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si))
		dispatch_signal((int)si.ssi_signo);
}

static void poll_signals(void)
{
	static const struct timespec zero = { 0, 0 };
	siginfo_t si;

	sfd = signalfd(-1, &sig_want, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd >= 0)
		log_note("signalfd: recovered, back to event-driven signals");

	while (sigtimedwait(&sig_want, &si, &zero) > 0)
		dispatch_signal(si.si_signo);
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
		    now - r->started < (long long)ng_start_ms(map, i)) {
			k++;
			continue;
		}

		if (r->ntf_fd >= 0) {
			uint32_t before = n_live;

			if (r->ntf_exec)
				drain_exec(i);
			else
				drain_notify(i, DRAIN_FINAL_CHUNKS);
			if (!r->starting || r->timedout || n_live != before) {
				k = 0;
				continue;
			}
		}

		log_err("%s: startup deadline of %u ms exceeded, giving up on it",
			ng_name(map, i), ng_start_ms(map, i));
		r->timedout = 1;
		if (state[i] == NG_ST_RUNNING)
			service_failed(i, FAIL_ST_TIMEOUT);
		else
			start_failed(i, FAIL_ST_TIMEOUT);
		k = 0;
	}
}

static const char *state_name(uint32_t i)
{
	switch (state[i]) {
	case NG_ST_PENDING:	return "pending";
	case NG_ST_RUNNING:	return "starting";
	case NG_ST_DONE:	return up[i] ? "up" : "down";
	case NG_ST_FAILED:	return "failed";
	case NG_ST_SKIPPED:	return "skipped";
	default:		return "?";
	}
}

static void ctl_init(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	if (mkdir(NINIT_CTL_DIR, 0700) < 0 && errno != EEXIST) {
		log_warn("control: mkdir %s: %s", NINIT_CTL_DIR, strerror(errno));
		return;
	}
	if (chmod(NINIT_CTL_DIR, 0700) < 0)
		log_warn("control: chmod %s: %s", NINIT_CTL_DIR, strerror(errno));

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		log_warn("control: socket: %s", strerror(errno));
		return;
	}
	memcpy(sa.sun_path, NINIT_CTL_SOCK, sizeof(NINIT_CTL_SOCK));
	unlink(NINIT_CTL_SOCK);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, CTL_MAX) < 0) {
		log_warn("control: bind %s: %s", NINIT_CTL_SOCK, strerror(errno));
		close(fd);
		return;
	}
	if (chmod(NINIT_CTL_SOCK, 0600) < 0)
		log_warn("control: chmod %s: %s", NINIT_CTL_SOCK, strerror(errno));
	ctl_lfd = fd;
	log_note("control: listening on %s", NINIT_CTL_SOCK);
}

static void ctl_out(struct ctl *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void ctl_out(struct ctl *c, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (c->done || c->out_len >= CTL_OUT - 128)
		return;
	va_start(ap, fmt);
	n = vsnprintf(c->out + c->out_len, CTL_OUT - c->out_len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= (size_t)(CTL_OUT - c->out_len))
		n = CTL_OUT - c->out_len - 1;
	c->out_len += (uint16_t)n;
}

static void ctl_end(struct ctl *c, int ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void ctl_end(struct ctl *c, int ok, const char *fmt, ...)
{
	char buf[CTL_BUF];
	va_list ap;

	if (c->done)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	c->listing = 0;
	c->svc = UINT32_MAX;
	ctl_out(c, "%s%s\n", ok ? NCTL_OK : NCTL_ERR, buf);
	c->done = 1;
}

static void ctl_drop(struct ctl *c)
{
	close(c->fd);
	*c = ctl_conn[--n_ctl];
}

static void ctl_fill(struct ctl *c)
{
	while (c->listing && c->out_len < CTL_OUT - 256) {
		uint32_t i = c->list_at;

		if (c->listing == 2) {
			const char *l = log_kept_line(i);

			if (!l) {
				c->listing = 0;
				ctl_end(c, 1, "%u retained failure line%s", i,
					i == 1 ? "" : "s");
				return;
			}
			c->list_at++;
			ctl_out(c, NCTL_DATA "%s\n", l);
			continue;
		}
		if (i >= n_svc) {
			c->listing = 0;
			ctl_end(c, 1, "%u services, %u up", n_svc, n_up);
			return;
		}
		c->list_at++;
		ctl_out(c, NCTL_DATA "%s %s %s pid %d\n", ng_name(map, i), state_name(i),
			want[i] ? "stopped" : "wanted", (int)runs[i].pid);
	}
}

// 0 while output is still pending, 1 once it is all written, -1 if the peer is gone
static int ctl_flush(struct ctl *c)
{
	while (c->out_at < c->out_len) {
		ssize_t k = write(c->fd, c->out + c->out_at,
				  (size_t)(c->out_len - c->out_at));

		if (k > 0) {
			c->out_at += (uint16_t)k;
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && errno == EAGAIN)
			return 0;
		return -1;
	}
	c->out_at = c->out_len = 0;
	return 1;
}

static void ctl_pump(struct ctl *c)
{
	int rc;

	ctl_fill(c);
	rc = ctl_flush(c);
	if (!rc)
		return;
	if (rc < 0 || c->done)
		ctl_drop(c);
}

static uint32_t ctl_find(const char *name)
{
	uint32_t i;

	for (i = 0; i < n_svc; i++)
		if (!strcmp(ng_name(map, i), name))
			return i;
	return UINT32_MAX;
}

// every connection watching this service learns how its operation ended
static void svc_op_set(uint32_t i, uint8_t op, long long at)
{
	if ((runs[i].op == SVC_OP_NONE) != (op == SVC_OP_NONE)) {
		if (op == SVC_OP_NONE)
			n_ops--;
		else
			n_ops++;
	}
	runs[i].op = op;
	runs[i].op_at = at;
}

static void svc_op_done(uint32_t i, int ok, const char *msg)
{
	int k;

	svc_op_set(i, SVC_OP_NONE, 0);
	runs[i].op_restart = 0;
	for (k = 0; k < n_ctl; k++)
		if (ctl_conn[k].svc == i)
			ctl_end(&ctl_conn[k], ok, "%s %s", ng_name(map, i), msg);
	maybe_free(i);
}

static void svc_op_start(uint32_t i)
{
	struct run *r = &runs[i];
	const char *why = "";

	want[i] = 0;
	if (!svc_can_start(i, &why)) {
		if (!shutting_down && r->pid <= 0 && !r->stale_pid) {
			r->attempt = 0;
			r->burst = 0;
			if (state[i] != NG_ST_PENDING) {
				state[i] = NG_ST_PENDING;
				n_pending++;
			}
		}
		svc_op_done(i, 0, why);
		return;
	}
	r->attempt = 0;
	r->burst = 0;
	r->op_restart = 0;
	if (state[i] != NG_ST_PENDING) {
		state[i] = NG_ST_PENDING;
		n_pending++;
	}
	if (!live_has(i))
		live_add(i);
	svc_op_set(i, SVC_OP_START, now_ms() + (long long)ng_start_ms(map, i) + KILL_GRACE_MS);
	spawn(i);
}

static void svc_op_stop(uint32_t i, int restart)
{
	struct run *r = &runs[i];

	want[i] = 1;
	r->restart_at = 0;
	r->op_restart = (uint8_t)restart;
	if (!live_has(i))
		live_add(i);

	if (svc_gone(i)) {
		if (restart) {
			svc_op_start(i);
			return;
		}
		svc_op_done(i, 1, "already stopped");
		return;
	}
	signal_group(i, SIGTERM);
	svc_op_set(i, SVC_OP_TERM, now_ms() + ng_stop_ms(map, i));
}

static void ctl_tick(void)
{
	long long now;
	uint32_t k;

	if (!n_ops)
		return;
	now = now_ms();
	for (k = 0; k < n_live; ) {
		uint32_t i = live[k];
		struct run *r = &runs[i];
		int gone = svc_gone(i);

		switch (r->op) {
		case SVC_OP_TERM:
			if (gone) {
				if (r->op_restart)
					svc_op_start(i);
				else
					svc_op_done(i, 1, "stopped");
				break;
			}
			if (now < r->op_at)
				break;
			log_warn("%s: still running %u ms after SIGTERM, killing it",
				 ng_name(map, i), ng_stop_ms(map, i));
			signal_group(i, SIGKILL);
			svc_op_set(i, SVC_OP_KILL, now + KILL_GRACE_MS);
			break;

		case SVC_OP_KILL:
			if (gone) {
				if (r->op_restart)
					svc_op_start(i);
				else
					svc_op_done(i, 1, "stopped, it needed SIGKILL");
				break;
			}
			if (now >= r->op_at)
				svc_op_done(i, 0, "would not stop, even after SIGKILL");
			break;

		case SVC_OP_START:
			if (state[i] == NG_ST_DONE && up[i]) {
				svc_op_done(i, 1, "up");
				break;
			}
			if (state[i] == NG_ST_FAILED || state[i] == NG_ST_SKIPPED) {
				svc_op_done(i, 0, state_name(i));
				break;
			}
			if (now >= r->op_at)
				svc_op_done(i, 0, "did not come up in time");
			break;

		default:
			break;
		}
		// finishing an operation swaps another service into this slot
		if (k < n_live && live[k] == i)
			k++;
	}
}

static void ctl_resume(struct ctl *c)
{
	uint32_t i, reset = 0, started = 0;

	for (i = 0; i < n_svc; i++) {
		if (state[i] != NG_ST_FAILED && state[i] != NG_ST_SKIPPED)
			continue;
		state[i] = NG_ST_PENDING;
		runs[i].attempt = 0;
		runs[i].burst = 0;
		n_pending++;
		reset++;
	}
	if (!reset) {
		ctl_end(c, 1, "nothing to resume");
		return;
	}
	for (i = 0; i < n_svc; i++) {
		if (state[i] != NG_ST_PENDING)
			continue;
		if (ng_svcs(map)[i].type == NG_TYPE_TARGET) {
			if (!unmet[i] && !want[i])
				complete(i);
			continue;
		}
		started += (uint32_t)svc_try_start(i);
	}
	log_note("control: resuming %u service%s, %u started", reset,
		 reset == 1 ? "" : "s", started);
	ctl_end(c, 1, "resuming %u service%s, %u started now", reset,
		reset == 1 ? "" : "s", started);
}

static void ctl_cmd(struct ctl *c, char *line)
{
	char *arg = strchr(line, ' ');
	uint32_t i;
	int restart;

	if (arg) {
		*arg++ = '\0';
		while (*arg == ' ')
			arg++;
		if (!*arg)
			arg = NULL;
	}

	if (!strcmp(line, "status")) {
		if (!arg) {
			c->listing = 1;
			c->list_at = 0;
			return;
		}
		i = ctl_find(arg);
		if (i == UINT32_MAX) {
			ctl_end(c, 0, "no service named %s", arg);
			return;
		}
		ctl_out(c, NCTL_DATA "%s %s %s pid %d\n", ng_name(map, i), state_name(i),
			want[i] ? "stopped" : "wanted", (int)runs[i].pid);
		ctl_end(c, 1, "%s", ng_name(map, i));
		return;
	}

	if (!strcmp(line, "log")) {
		if (!log_kept()) {
			ctl_end(c, 1, "no failures recorded");
			return;
		}
		c->listing = 2;
		c->list_at = 0;
		return;
	}

	if (!strcmp(line, "reload")) {
		ctl_end(c, 0, "the running graph cannot be replaced; reboot to load a new one");
		return;
	}

	if (!strcmp(line, "resume")) {
		if (shutting_down)
			ctl_end(c, 0, "shutting down");
		else
			ctl_resume(c);
		return;
	}

	restart = !strcmp(line, "restart");
	if (!restart && strcmp(line, "stop") && strcmp(line, "start")) {
		ctl_end(c, 0, "unknown command '%s' "
			"(status, log, start, stop, restart, resume, reload)", line);
		return;
	}
	if (!arg) {
		ctl_end(c, 0, "%s needs a service name", line);
		return;
	}
	if (shutting_down) {
		ctl_end(c, 0, "shutting down");
		return;
	}
	i = ctl_find(arg);
	if (i == UINT32_MAX) {
		ctl_end(c, 0, "no service named %s", arg);
		return;
	}
	if (ng_svcs(map)[i].type == NG_TYPE_TARGET) {
		ctl_end(c, 0, "%s is a target, it has no process", arg);
		return;
	}
	if (runs[i].op != SVC_OP_NONE) {
		ctl_end(c, 0, "%s is busy with another operation", arg);
		return;
	}

	log_note("control: %s %s", line, ng_name(map, i));
	c->svc = i;
	if (restart)
		svc_op_stop(i, 1);
	else if (!strcmp(line, "stop"))
		svc_op_stop(i, 0);
	else
		svc_op_start(i);
}

static void ctl_read(struct ctl *c)
{
	char *nl;
	ssize_t k;

	// a connection with nothing to say still has to notice a hangup, or the
	// dead socket stays ready and the loop spins until its operation ends
	if (c->done || c->listing || c->svc != UINT32_MAX) {
		char skip[256];

		k = read(c->fd, skip, sizeof(skip));
		if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR))
			ctl_drop(c);
		return;
	}

	k = read(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len - 1);
	if (k == 0) {
		ctl_drop(c);
		return;
	}
	if (k < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		ctl_drop(c);
		return;
	}
	c->in_len += (uint16_t)k;
	c->in[c->in_len] = '\0';

	nl = memchr(c->in, '\n', c->in_len);
	if (!nl) {
		if (c->in_len >= sizeof(c->in) - 1) {
			ctl_end(c, 0, "request too long");
			ctl_pump(c);
		}
		return;
	}
	*nl = '\0';
	c->in[strcspn(c->in, "\r")] = '\0';
	ctl_cmd(c, c->in);
	c->in_len = 0;
	ctl_pump(c);
}

static void ctl_accept(void)
{
	for (;;) {
		int fd = accept4(ctl_lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		struct ctl *c;

		if (fd < 0)
			return;
		if (n_ctl == CTL_MAX) {
			(void)!write(fd, NCTL_ERR "too many control connections\n", 32);
			close(fd);
			continue;
		}
		c = &ctl_conn[n_ctl++];
		memset(c, 0, sizeof(*c));
		c->fd = fd;
		c->svc = UINT32_MAX;
	}
}

static long long ctl_due(void)
{
	long long best = -1, now, d;
	uint32_t k;

	if (!n_ops)
		return -1;
	now = now_ms();
	for (k = 0; k < n_live; k++) {
		const struct run *r = &runs[live[k]];

		if (r->op == SVC_OP_NONE)
			continue;
		d = r->op_at - now;
		if (d < 0)
			d = 0;
		if (best < 0 || d < best)
			best = d;
	}
	return best;
}

static void check_boot_done(void)
{
	uint32_t k;

	if (boot_reported || shutting_down || !n_svc || n_active)
		return;
	for (k = 0; k < n_live; k++)
		if (runs[live[k]].restart_at || runs[live[k]].stale_pid)
			return;
	boot_reported = 1;
	fail_summary(map, state);
	if (n_pending)
		log_warn("boot: %u service%s never started, waiting on something that is not up",
			 n_pending, n_pending == 1 ? "" : "s");
	log_note("boot: %u of %u services up in %lld ms", n_done, n_svc, now_ms() - boot_t0);
}

static void start_graph(void)
{
	const struct ng_hdr *h = map;
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

	for (pid_mask = 64; pid_mask < 4 * n_svc; pid_mask *= 2)
		;
	pid_key = calloc(pid_mask, sizeof(*pid_key));
	pid_val = calloc(pid_mask, sizeof(*pid_val));
	pid_mask--;
	if (!pid_key || !pid_val) {
		free(pid_key);
		free(pid_val);
		pid_key = NULL;
		pid_val = NULL;
	}

	if (!state || !up || !want || !released || !rqueue || !unmet || !runs ||
	    !live || !queue) {
		n_svc = 0;
		n_pending = 0;
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
			svc_try_start(i);
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
	cgroup_init();
	ctl_init();

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

	pfds = calloc(3 + CTL_MAX + 2 * (size_t)n_svc, sizeof(*pfds));
	pf_idx = calloc(3 + CTL_MAX + 2 * (size_t)n_svc, sizeof(*pf_idx));
	pf_kind = calloc(3 + CTL_MAX + 2 * (size_t)n_svc, sizeof(*pf_kind));
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
		static long long last_stall = -1;
		long long wait, due;
		nfds_t nfds = 1, k;
		uint32_t start;
		int rc;

		log_batch_begin();
		drain_left = DRAIN_BUDGET;
		fire_restarts();
		check_timeouts();
		ctl_tick();
		for (int c = n_ctl; c-- > 0; )
			if (ctl_conn[c].out_at < ctl_conn[c].out_len ||
			    ctl_conn[c].listing || ctl_conn[c].done)
				ctl_pump(&ctl_conn[c]);
		check_boot_done();
		fail_emergency_tick();

		if (sfd < 0)
			poll_signals();
		if (degraded) {
			drain_all();
			if (fail_emergency_fd() >= 0)
				fail_emergency_report();
		}

		pfds[0].fd = sfd;
		pfds[0].events = POLLIN;
		start = n_live && live_has(drain_rotor) ? runs[drain_rotor].live_pos : 0;
		for (k = 0; !degraded && k < n_live; k++, start++) {
			uint32_t i;

			if (start >= n_live)
				start = 0;
			i = live[start];

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
				pf_kind[nfds++] = runs[i].ntf_exec ? 2 : 1;
			}
		}

		if (!degraded && fail_emergency_fd() >= 0) {
			pfds[nfds].fd = fail_emergency_fd();
			pfds[nfds].events = POLLIN;
			pf_idx[nfds] = 0;
			pf_kind[nfds++] = 3;
		}

		if (!degraded && ctl_lfd >= 0) {
			pfds[nfds].fd = ctl_lfd;
			pfds[nfds].events = POLLIN;
			pf_idx[nfds] = 0;
			pf_kind[nfds++] = 4;
			for (int c = 0; c < n_ctl; c++) {
				struct ctl *cc = &ctl_conn[c];

				pfds[nfds].fd = cc->fd;
				pfds[nfds].events = POLLIN;
				if (cc->out_at < cc->out_len || cc->listing)
					pfds[nfds].events |= POLLOUT;
				pf_idx[nfds] = (uint32_t)c;
				pf_kind[nfds++] = 5;
			}
		}

		wait = n_active && !shutting_down ? STALL_MS : -1;
		due = restarts_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		due = starts_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		due = fail_emergency_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		due = ctl_due();
		if (due >= 0 && (wait < 0 || due < wait))
			wait = due;
		if (degraded && (wait < 0 || wait > DEGRADED_POLL_MS))
			wait = DEGRADED_POLL_MS;
		if (sfd < 0 && (wait < 0 || wait > SIGNAL_POLL_MS))
			wait = SIGNAL_POLL_MS;

		rc = poll(pfds, nfds, (int)wait);
		if (rc < 0) {
			if (errno != EINTR) {
				log_err("poll: %s", strerror(errno));
				poll(NULL, 0, 1000);
			}
			continue;
		}
		if (rc == 0) {
			long long t = now_ms();

			if (last_stall < 0)
				last_stall = t;
			else if (t - last_stall >= STALL_MS) {
				last_stall = t;
				report_stalls();
			}
			continue;
		}

		if (pfds[0].revents)
			handle_signals();

		for (k = 1; k < nfds; k++) {
			uint32_t i = pf_idx[k];

			if (!pfds[k].revents)
				continue;
			if (pf_kind[k] == 3) {
				fail_emergency_report();
				continue;
			}
			if (pf_kind[k] == 4) {
				ctl_accept();
				continue;
			}
			if (pf_kind[k] == 5) {
				// a reply may have closed and reshuffled the slots
				if (i >= (uint32_t)n_ctl || ctl_conn[i].fd != pfds[k].fd)
					continue;
				if (pfds[k].revents & (POLLOUT | POLLERR | POLLHUP))
					ctl_pump(&ctl_conn[i]);
				if (i < (uint32_t)n_ctl && ctl_conn[i].fd == pfds[k].fd &&
				    (pfds[k].revents & (POLLIN | POLLERR | POLLHUP)))
					ctl_read(&ctl_conn[i]);
				continue;
			}
			if (pf_kind[k] == 2)
				drain_exec(i);
			else if (pf_kind[k])
				drain_notify(i, DRAIN_POLL_CHUNKS);
			else
				drain_out(i, DRAIN_POLL_CHUNKS, 1);
			if (live_has(i))
				maybe_free(i);
		}
	}
}
