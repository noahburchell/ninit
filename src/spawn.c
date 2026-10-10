#include "spawn.h"
#include "boot.h"
#include "cgroup.h"
#include "fail.h"
#include "logging.h"
#include "ngraph.h"
#include "ninit.h"
#include "pidmap.h"
#include "reap.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CLONE_INTO_CGROUP
#define CLONE_INTO_CGROUP 0x200000000ULL
#endif

struct ninit_clone_args {
	uint64_t flags, pidfd, child_tid, parent_tid, exit_signal;
	uint64_t stack, stack_size, tls, set_tid, set_tid_size, cgroup;
};

static_assert(sizeof(struct ninit_clone_args) == 88, "clone_args v2 is 88 bytes");

static int clone3_ok = 1;

static_assert(sizeof(NG_SHELL) <= NG_MAX_INTERP + 1, "the exec failure message holds the shell path");

static void child_exec(uint32_t i, int out_w, int ntf_w, const char *cg) __attribute__((noreturn));

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

static void child_exec(uint32_t i, int out_w, int ntf_w, const char *cg)
{
	static char env_path[] = NG_PATH, env_home[] = "HOME=/", env_term[] = "TERM=linux";
	static char *envp[4 + NG_LOCALE_MAX];
	static char arg_shell[] = NG_SHELL_ARGV0, arg_c[] = "-c";
	char *argv[NG_MAX_EXEC_ARGS + 2];
	const char *prog = NG_SHELL;
	int e = 0;
	char msg[NG_MAX_INTERP + 64];
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
	if (setrlimit(RLIMIT_NOFILE, &child_nofile) < 0) {
		at = put_str(msg, 0, "ninit: could not set its descriptor limit: errno ");
		at = put_num(msg, at, (unsigned)errno);
		at = put_str(msg, at, ", it runs with pid 1's\n");
		(void)!write(2, msg, at);
	}

	// oom_score_adj is inherited, and only pid 1 may be exempt
	ninit_oom_score_adj("0\n", 2);

	(void)!chdir("/");
	umask(022);

	if (ng_interp(map, i)) {
		const char *p = ng_exec(map, i);
		size_t k = 0;

		for (; *p; p += strlen(p) + 1)
			argv[k++] = (char *)(uintptr_t)p;
		argv[k++] = (char *)(uintptr_t)ng_script(map, i);
		for (p++; *p; p += strlen(p) + 1)
			argv[k++] = (char *)(uintptr_t)p;
		argv[k] = NULL;
		prog = argv[0];
	} else {
		argv[0] = arg_shell;
		argv[1] = arg_c;
		argv[2] = (char *)(uintptr_t)ng_script(map, i);
		argv[3] = (char *)(uintptr_t)ng_name(map, i);
		argv[4] = NULL;
	}
	execve(prog, argv, envp);
	err = errno;

	at = put_str(msg, 0, "ninit: exec ");
	at = put_str(msg, at, prog);
	at = put_str(msg, at, ": errno ");
	at = put_num(msg, at, (unsigned)err);
	at = put_str(msg, at, "\n");
	(void)!write(2, msg, at);
	if (ntf_w >= 0 && !nfd)
		(void)!write(ntf_w, "x", 1);
	_exit(127);
}

static pid_t spawn_into_cgroup(uint32_t i, int have_cg)
{
#ifdef SYS_clone3
	struct ninit_clone_args ca = { .flags = CLONE_INTO_CGROUP, .exit_signal = SIGCHLD };
	char dir[CG_PATH_MAX];
	long pid;
	int fd, err;

	if (!clone3_ok || !have_cg || !cg_path(i, "", dir, sizeof(dir)))
		return -1;
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	ca.cgroup = (uint64_t)fd;
	pid = syscall(SYS_clone3, &ca, sizeof(ca));
	if (pid == 0)
		return 0;
	if (pid > 0) {
		close(fd);
		return (pid_t)pid;
	}

	err = errno;
	close(fd);
	if (err == ENOSYS || err == EINVAL || err == E2BIG || err == EOPNOTSUPP) {
		clone3_ok = 0;
		log_warn("cgroup: this kernel cannot fork into a cgroup, joining after fork instead");
	}
	return -1;
#else
	(void)i;
	(void)have_cg;
	return -1;
#endif
}

int launch(uint32_t i)
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

	// linux before 7.3 kills a child cloned into a cgroup that cgroup.kill was written to,
	// a new cgroup or a join after fork is not affected
	if (r->cg_killed && cgroup_drop(i))
		r->cg_killed = 0;
	cgroup_make(i, cg, sizeof(cg));

	pid = spawn_into_cgroup(i, cg[0] != '\0' && !r->cg_killed);
	if (pid == 0)
		child_exec(i, out[1], ntf[1], "");
	if (pid < 0) {
		pid = fork();
		if (pid == 0)
			child_exec(i, out[1], ntf[1], cg);
	}
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

void spawn(uint32_t i)
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
