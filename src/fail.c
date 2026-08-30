#define _GNU_SOURCE

#include "fail.h"
#include "logging.h"
#include "ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

static void describe(int status, char *buf, size_t cap)
{
	if (WIFEXITED(status))
		snprintf(buf, cap, "exit %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		snprintf(buf, cap, "killed by SIG%s", sigabbrev_np(WTERMSIG(status)));
	else
		snprintf(buf, cap, "status %#x", (unsigned)status);
}

int fail_service(const void *map, uint32_t i, int status, unsigned attempt,
		 const char *tail, size_t tail_len)
{
	const struct ng_svc *s = &ng_svcs(map)[i];
	const char *name = ng_name(map, i);
	char how[64];

	describe(status, how, sizeof(how));

	if (attempt < 2) {
		log_warn("%s failed (%s), retrying once", name, how);
		return -1;
	}

	log_err("%s failed twice (%s)", name, how);
	if (tail_len)
		log_raw(LOG_ERR, tail, tail_len);

	switch (ng_onfail(map, i)) {
	case NG_ONFAIL_WARN:
		log_warn("nothing depends on %s -- continuing without it", name);
		return NG_ONFAIL_WARN;
	case NG_ONFAIL_SHELL:
		log_err("%s takes down %u of %u services", name, s->n_desc,
			((const struct ng_hdr *)map)->n_svc);
		return NG_ONFAIL_SHELL;
	default:
		log_err("%u service(s) depend on %s and will not start", s->n_desc, name);
		return NG_ONFAIL_STOP;
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

void fail_emergency_shell(const char *why)
{
	pid_t pid;
	int fd;

	log_err("%s", why);
	log_err("starting an emergency shell on the console");

	pid = fork();
	if (pid < 0) {
		log_err("fork for emergency shell: %s", strerror(errno));
		return;
	}
	if (pid > 0)
		return;

	setsid();
	fd = open("/dev/console", O_RDWR | O_NOCTTY);
	if (fd >= 0) {
		ioctl(fd, TIOCSCTTY, 1);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
	execl("/bin/sh", "-sh", (char *)NULL);
	_exit(127);
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
