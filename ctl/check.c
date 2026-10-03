#include "init.h"
#include "parser/parser.h"
#include "util.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

void check_syntax(struct src *srcs, uint32_t n, const char *dir)
{
	static char argv0[] = NG_SHELL_ARGV0, dashn[] = "-n", dasho[] = "-O", extglob[] = "extglob";
	static char path[] = NG_PATH;
	static char *const envp[] = { path, NULL };
	static char *const cargv[] = { argv0, dashn, NULL };
	// -n never runs the shopt that turns extglob on, so bash would refuse its patterns
	static char *const bargv[] = { argv0, dasho, extglob, dashn, NULL };
	char *const *args = strcmp(strrchr(NG_SHELL, '/') + 1, "bash") ? cargv : bargv;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	uint32_t slots = ncpu > 1 ? (uint32_t)ncpu : 1;
	uint32_t i, live = 0, bad = 0, unchecked = 0;
	int giveup = 0;
	pid_t *pids;
	int *errfd;
	uint32_t *who;
	char **failed;
	int devnull;

	if (access(NG_SHELL, X_OK) != 0) {
		fprintf(stderr, "ninitctl: warning: %s is not executable, "
			"skipping the syntax check\n", NG_SHELL);
		return;
	}
	if (slots > 32)
		slots = 32;
	pids = xmalloc(slots * sizeof(*pids));
	errfd = xmalloc(slots * sizeof(*errfd));
	who = xmalloc(slots * sizeof(*who));
	// checks end in any order, their messages print in service order
	failed = xmalloc(n * sizeof(*failed));
	devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);

	for (i = 0; i <= n; i++) {
		size_t len;
		int sfd, efd;
		pid_t pid;

		while (live == slots || ((i == n || giveup) && live)) {
			int st;
			pid_t got = wait(&st);
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
			if (!WIFEXITED(st) || WEXITSTATUS(st)) {
				char msg[4096];
				ssize_t mn;
				size_t ml;

				bad++;
				lseek(errfd[k], 0, SEEK_SET);
				mn = read(errfd[k], msg, sizeof(msg) - 1);
				ml = mn > 0 ? (size_t)mn : 0;
				msg[ml] = '\0';
				failed[who[k]] = xmalloc(ml + 1);
				memcpy(failed[who[k]], msg, ml + 1);
			}
			close(errfd[k]);
			live--;
			pids[k] = pids[live];
			errfd[k] = errfd[live];
			who[k] = who[live];
		}
		if (i == n)
			break;
		if (!srcs[i].script)
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

		pid = fork();
		if (pid < 0)
			die("fork: %s", strerror(errno));
		if (pid == 0) {
			dup2(sfd, 0);
			if (devnull >= 0)
				dup2(devnull, 1);
			dup2(efd, 2);
			execve(NG_SHELL, args, envp);
			_exit(127);
		}
		close(sfd);
		pids[live] = pid;
		errfd[live] = efd;
		who[live] = i;
		live++;
	}

	if (devnull >= 0)
		close(devnull);
	for (i = 0; i < n; i++) {
		if (failed[i])
			print_failure(dir, srcs[i].name, failed[i]);
		free(failed[i]);
	}
	free(failed);
	free(pids);
	free(errfd);
	free(who);
	if (bad)
		die("%u service script%s failed the %s syntax check",
		    bad, bad == 1 ? "" : "s", NG_SHELL);
	if (unchecked)
		die("%u service script%s could not be checked, use --no-check to skip", unchecked,
		    unchecked == 1 ? "" : "s");
}
