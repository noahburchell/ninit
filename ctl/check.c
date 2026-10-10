#include "init.h"
#include "parser/parser.h"
#include "util.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CHECK_MS
#define CHECK_MS	30000
#endif

// the shell saw a memfd, so each line gets the path of the service
static void print_failure(const char *dir, const char *name, char *msg)
{
	char *q = msg, *nl;

	while (q) {
		nl = strchr(q, '\n');
		if (nl)
			*nl++ = '\0';
		if (*q)
			fprintf(stderr, "ninitctl: %s/%s: %s\n", dir, name, q);
		q = nl;
	}
}

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// waits for a check to end, killing each one still running at its deadline, due -1 marks it
static pid_t reap_check(const pid_t *pids, long long *due, uint32_t live, const sigset_t *chld, int *st)
{
	for (;;) {
		pid_t got = waitpid(-1, st, WNOHANG);
		long long now, next = LLONG_MAX;
		struct timespec ts;

		if (got)
			return got;
		now = mono_ms();
		for (uint32_t k = 0; k < live; k++) {
			if (due[k] < 0)
				continue;
			if (due[k] <= now) {
				kill(pids[k], SIGKILL);
				due[k] = -1;
			} else if (due[k] < next) {
				next = due[k];
			}
		}
		if (next == LLONG_MAX)
			next = now + 1000;
		ts.tv_sec = (time_t)((next - now) / 1000);
		ts.tv_nsec = (long)((next - now) % 1000 * 1000000);
		sigtimedwait(chld, NULL, &ts);
	}
}

// a check for another root runs inside it, which needs CAP_SYS_CHROOT
static int chroot_errno(void)
{
	pid_t pid = fork();
	int st;

	if (pid < 0)
		return errno;
	if (pid == 0)
		_exit(chroot(g_root) < 0 ? errno : 0);
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return errno;
	return WIFEXITED(st) ? WEXITSTATUS(st) : EPERM;
}

void check_syntax(struct src *srcs, uint32_t n, const char *dir)
{
	static char path[] = NG_PATH;
	static char *const envp[] = { path, NULL };
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	uint32_t slots = ncpu > 1 ? (uint32_t)ncpu : 1;
	uint32_t i, live = 0, bad = 0, unchecked = 0;
	const char *what = NULL;
	int shell_ok = 1;
	int giveup = 0;
	pid_t *pids;
	long long *due;
	int *errfd;
	uint32_t *who;
	char **failed;
	uint8_t *late;
	int devnull, e;
	struct sigaction dfl = { .sa_handler = SIG_DFL }, old_chld;
	sigset_t chld, old_mask;
	struct stat sh;

	// an interpreter #! names was found runnable when the file was read, the shell was not
	if (sys_stat(NG_SHELL, &sh) < 0 || sys_exec_ok(NG_SHELL, &sh) < 0) {
		fprintf(stderr, "ninitctl: warning: %s%s is not executable, "
			"skipping the syntax check of shell scripts\n", g_root_pfx, NG_SHELL);
		shell_ok = 0;
	}
	if (slots > 32)
		slots = 32;
	pids = xmalloc(slots * sizeof(*pids));
	due = xmalloc(slots * sizeof(*due));
	errfd = xmalloc(slots * sizeof(*errfd));
	who = xmalloc(slots * sizeof(*who));
	// checks end in any order, their messages print in service order
	failed = xmalloc(n * sizeof(*failed));
	late = xmalloc(n);
	devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
	// an ignored SIGCHLD would reap the checks before waitpid sees them
	sigemptyset(&chld);
	sigaddset(&chld, SIGCHLD);
	sigaction(SIGCHLD, &dfl, &old_chld);
	sigprocmask(SIG_BLOCK, &chld, &old_mask);
	if (g_root && (e = chroot_errno())) {
		fprintf(stderr, "ninitctl: warning: chroot %s: %s, skipping the syntax check\n",
			g_root, strerror(e));
		giveup = 1;
	}

	for (i = 0; i <= n; i++) {
		char *argv[16];
		const char *prog;
		size_t len;
		int sfd, efd;
		pid_t pid;

		while (live == slots || ((i == n || giveup) && live)) {
			int st;
			pid_t got = reap_check(pids, due, live, &chld, &st);
			uint32_t k;

			if (got < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "ninitctl: warning: wait: %s\n",
					strerror(errno));
				for (k = 0; k < live; k++)
					close(errfd[k]);
				unchecked += live;
				live = 0;
				giveup = 1;
				break;
			}
			for (k = 0; k < live; k++)
				if (pids[k] == got)
					break;
			if (k == live)
				continue;
			if (due[k] < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) {
				char msg[4096];
				ssize_t mn;
				size_t ml = 0;

				if (due[k] < 0) {
					late[who[k]] = 1;
					unchecked++;
					ml = (size_t)snprintf(msg, sizeof(msg),
							      "the syntax check did not finish in %d s\n",
							      CHECK_MS / 1000);
				} else {
					bad++;
				}
				lseek(errfd[k], 0, SEEK_SET);
				mn = read(errfd[k], msg + ml, sizeof(msg) - 1 - ml);
				ml += mn > 0 ? (size_t)mn : 0;
				msg[ml] = '\0';
				failed[who[k]] = xmalloc(ml + 1);
				memcpy(failed[who[k]], msg, ml + 1);
			}
			close(errfd[k]);
			live--;
			pids[k] = pids[live];
			due[k] = due[live];
			errfd[k] = errfd[live];
			who[k] = who[live];
		}
		if (i == n)
			break;
		if (!srcs[i].script || (srcs[i].lang == &lang_shell && !shell_ok))
			continue;
		if (giveup) {
			unchecked++;
			continue;
		}

		len = strlen(srcs[i].script);
		sfd = memfd_create(srcs[i].name, MFD_CLOEXEC);
		efd = memfd_create("stderr", MFD_CLOEXEC);
		if (sfd < 0 || efd < 0) {
			if (sfd >= 0)
				close(sfd);
			if (efd >= 0)
				close(efd);
			fprintf(stderr, "ninitctl: warning: memfd_create: %s, "
				"skipping the syntax check\n", strerror(errno));
			giveup = 1;
			unchecked++;
			continue;
		}
		if ((size_t)write(sfd, srcs[i].script, len) != len ||
		    lseek(sfd, 0, SEEK_SET) != 0) {
			fprintf(stderr, "ninitctl: warning: %s/%s: cannot stage the script "
				"for the syntax check: %s\n", dir, srcs[i].name,
				strerror(errno));
			close(sfd);
			close(efd);
			unchecked++;
			continue;
		}

		prog = srcs[i].lang->check_argv(&srcs[i], argv, sizeof(argv) / sizeof(*argv));
		pid = fork();
		if (pid < 0)
			die("fork: %s", strerror(errno));
		if (pid == 0) {
			sigprocmask(SIG_SETMASK, &old_mask, NULL);
			if (g_root && (chroot(g_root) < 0 || chdir("/") < 0))
				_exit(127);
			dup2(sfd, 0);
			if (devnull >= 0)
				dup2(devnull, 1);
			dup2(efd, 2);
			execve(prog, argv, envp);
			_exit(127);
		}
		close(sfd);
		pids[live] = pid;
		due[live] = mono_ms() + CHECK_MS;
		errfd[live] = efd;
		who[live] = i;
		live++;
	}

	sigprocmask(SIG_SETMASK, &old_mask, NULL);
	sigaction(SIGCHLD, &old_chld, NULL);
	if (devnull >= 0)
		close(devnull);
	for (i = 0; i < n; i++) {
		if (!failed[i])
			continue;
		print_failure(dir, srcs[i].name, failed[i]);
		free(failed[i]);
		if (late[i])
			continue;
		if (!what)
			what = srcs[i].interp;
		else if (strcmp(what, srcs[i].interp))
			what = "";
	}
	free(failed);
	free(late);
	free(pids);
	free(due);
	free(errfd);
	free(who);
	if (bad)
		die("%u service script%s failed the %s%ssyntax check", bad, bad == 1 ? "" : "s",
		    what && *what ? what : "", what && *what ? " " : "");
	if (unchecked)
		die("%u service script%s could not be checked, use --no-check to skip", unchecked,
		    unchecked == 1 ? "" : "s");
}
