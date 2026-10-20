#include "init.h"
#include "parser/parser.h"
#include "util.h"
#include "../help/time.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
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

struct slot {
	pid_t pid;
	int errfd;
	uint32_t who;
	long long due;
};

struct checks {
	struct slot *slot;
	char **failed;
	uint8_t *late;
	uint32_t nslot, live, bad, unchecked;
	int devnull, giveup, userns;
	sigset_t chld, old_mask;
};

// a new user namespace grants CAP_SYS_CHROOT within it, chroot must come before any exec drops it
static int enter_root(int userns)
{
	if (userns && unshare(CLONE_NEWUSER) < 0)
		return -1;
	return chroot(g_root) < 0 || chdir("/") < 0 ? -1 : 0;
}

// waits for a check to end, killing each one still running at its deadline, due -1 marks it
static pid_t reap_check(struct checks *c, int *st)
{
	for (;;) {
		pid_t got = waitpid(-1, st, WNOHANG);
		long long now, next = LLONG_MAX;
		struct timespec ts;

		if (got)
			return got;
		now = mono_ms();
		for (uint32_t k = 0; k < c->live; k++) {
			struct slot *s = &c->slot[k];

			if (s->due < 0)
				continue;
			if (s->due <= now) {
				kill(s->pid, SIGKILL);
				s->due = -1;
			} else if (s->due < next) {
				next = s->due;
			}
		}
		if (next == LLONG_MAX)
			next = now + 1000;
		ts.tv_sec = (time_t)((next - now) / 1000);
		ts.tv_nsec = (long)((next - now) % 1000 * 1000000);
		sigtimedwait(&c->chld, NULL, &ts);
	}
}

// keeps the messages of a check that failed or did not finish
static void collect_one(struct checks *c)
{
	char msg[4096];
	struct slot *s;
	ssize_t mn;
	size_t ml = 0;
	uint32_t k;
	pid_t got;
	int st;

	for (;;) {
		got = reap_check(c, &st);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "ninitctl: warning: wait: %s\n", strerror(errno));
			for (k = 0; k < c->live; k++)
				close(c->slot[k].errfd);
			c->unchecked += c->live;
			c->live = 0;
			c->giveup = 1;
			return;
		}
		for (k = 0; k < c->live; k++)
			if (c->slot[k].pid == got)
				break;
		if (k < c->live)
			break;
	}
	s = &c->slot[k];
	if (s->due < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) {
		if (s->due < 0) {
			c->late[s->who] = 1;
			c->unchecked++;
			ml = (size_t)snprintf(msg, sizeof(msg), "the syntax check did not finish in %d s\n",
					      CHECK_MS / 1000);
		} else {
			c->bad++;
		}
		lseek(s->errfd, 0, SEEK_SET);
		mn = read(s->errfd, msg + ml, sizeof(msg) - 1 - ml);
		ml += mn > 0 ? (size_t)mn : 0;
		msg[ml] = '\0';
		c->failed[s->who] = xmalloc(ml + 1);
		memcpy(c->failed[s->who], msg, ml + 1);
	}
	close(s->errfd);
	*s = c->slot[--c->live];
}

static void start_one(struct checks *c, struct src *s, uint32_t i, const char *dir)
{
	static char path[] = NG_PATH;
	static char *const envp[] = { path, NULL };
	size_t len = strlen(s->script);
	char *argv[16];
	const char *prog;
	int sfd, efd;
	pid_t pid;

	sfd = memfd_create(s->name, MFD_CLOEXEC);
	efd = memfd_create("stderr", MFD_CLOEXEC);
	if (sfd < 0 || efd < 0) {
		if (sfd >= 0)
			close(sfd);
		if (efd >= 0)
			close(efd);
		fprintf(stderr, "ninitctl: warning: memfd_create: %s, "
			"skipping the syntax check\n", strerror(errno));
		c->giveup = 1;
		c->unchecked++;
		return;
	}
	if ((size_t)write(sfd, s->script, len) != len || lseek(sfd, 0, SEEK_SET) != 0) {
		fprintf(stderr, "ninitctl: warning: %s/%s: cannot stage the script "
			"for the syntax check: %s\n", dir, s->name, strerror(errno));
		close(sfd);
		close(efd);
		c->unchecked++;
		return;
	}

	prog = s->lang->check_argv(s, argv, sizeof(argv) / sizeof(*argv));
	pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (pid == 0) {
		sigprocmask(SIG_SETMASK, &c->old_mask, NULL);
		if (g_root && enter_root(c->userns) < 0)
			_exit(127);
		dup2(sfd, 0);
		if (c->devnull >= 0)
			dup2(c->devnull, 1);
		dup2(efd, 2);
		execve(prog, argv, envp);
		_exit(127);
	}
	close(sfd);
	c->slot[c->live++] = (struct slot){ .pid = pid, .errfd = efd, .who = i, .due = mono_ms() + CHECK_MS };
}

static void report(struct checks *c, struct src *srcs, uint32_t n, const char *dir)
{
	const char *what = NULL;

	for (uint32_t i = 0; i < n; i++) {
		if (!c->failed[i])
			continue;
		print_failure(dir, srcs[i].name, c->failed[i]);
		free(c->failed[i]);
		if (c->late[i])
			continue;
		if (!what)
			what = srcs[i].interp;
		else if (strcmp(what, srcs[i].interp))
			what = "";
	}
	free(c->failed);
	free(c->late);
	free(c->slot);
	if (c->bad)
		die("%u service script%s failed the %s%ssyntax check", c->bad, c->bad == 1 ? "" : "s",
		    what && *what ? what : "", what && *what ? " " : "");
	if (c->unchecked)
		die("%u service script%s could not be checked, use --no-check to skip", c->unchecked,
		    c->unchecked == 1 ? "" : "s");
}

// a check for another root runs inside it, through a user namespace when chroot alone is refused
static int root_errno(int *userns)
{
	int e = 0;

	for (int u = 0; u < 2; u++) {
		pid_t pid = fork();
		int st, r;

		if (pid < 0)
			return errno;
		if (pid == 0)
			_exit(enter_root(u) < 0 ? errno : 0);
		while (waitpid(pid, &st, 0) < 0)
			if (errno != EINTR)
				return errno;
		r = WIFEXITED(st) ? WEXITSTATUS(st) : EPERM;
		if (!r) {
			*userns = u;
			return 0;
		}
		if (!u)
			e = r;
	}
	return e;
}

void check_syntax(struct src *srcs, uint32_t n, const char *dir)
{
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	struct checks c = { .nslot = ncpu > 1 ? (uint32_t)ncpu : 1 };
	struct sigaction dfl = { .sa_handler = SIG_DFL }, old_chld;
	struct stat sh;
	int shell_ok = 1, e;

	// an interpreter #! names was found runnable when the file was read, the shell was not
	if (sys_stat(NG_SHELL, &sh) < 0 || sys_exec_ok(NG_SHELL, &sh) < 0) {
		fprintf(stderr, "ninitctl: warning: %s%s is not executable, "
			"skipping the syntax check of shell scripts\n", g_root_pfx, NG_SHELL);
		shell_ok = 0;
	}
	if (c.nslot > 32)
		c.nslot = 32;
	c.slot = xmalloc(c.nslot * sizeof(*c.slot));
	// checks end in any order, their messages print in service order
	c.failed = xmalloc(n * sizeof(*c.failed));
	c.late = xmalloc(n);
	c.devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
	// an ignored SIGCHLD would reap the checks before waitpid sees them
	sigemptyset(&c.chld);
	sigaddset(&c.chld, SIGCHLD);
	sigaction(SIGCHLD, &dfl, &old_chld);
	sigprocmask(SIG_BLOCK, &c.chld, &c.old_mask);
	if (g_root && (e = root_errno(&c.userns))) {
		fprintf(stderr, "ninitctl: warning: chroot %s: %s, skipping the syntax check\n",
			g_root, strerror(e));
		c.giveup = 1;
	}

	for (uint32_t i = 0; i < n; i++) {
		while (c.live == c.nslot || (c.giveup && c.live))
			collect_one(&c);
		if (!srcs[i].script || (srcs[i].lang == &lang_shell && !shell_ok))
			continue;
		if (c.giveup) {
			c.unchecked++;
			continue;
		}
		start_one(&c, &srcs[i], i, dir);
	}
	while (c.live)
		collect_one(&c);

	sigprocmask(SIG_SETMASK, &c.old_mask, NULL);
	sigaction(SIGCHLD, &old_chld, NULL);
	if (c.devnull >= 0)
		close(c.devnull);
	report(&c, srcs, n, dir);
}
