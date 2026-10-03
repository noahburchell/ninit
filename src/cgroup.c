#include "cgroup.h"
#include "logging.h"
#include "ngraph.h"
#include "ninit.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CG_MAGIC	0x63677270

int cg_ok;

void cgroup_init(void)
{
	struct statfs sf;

	if (statfs(CG_BASE, &sf) < 0 || (unsigned long)sf.f_type != CG_MAGIC) {
		log_warn("cgroup: %s is not a cgroup2 mount, using process groups", CG_BASE);
		return;
	}
	if (mkdir(CG_DIR, 0755) < 0 && errno != EEXIST) {
		log_warn("cgroup: mkdir %s: %s", CG_DIR, strerror(errno));
		return;
	}
	if (mkdir(CG_DIR "/.probe", 0755) < 0 && errno != EEXIST) {
		log_warn("cgroup: cannot create %s/*: %s, using process groups",
			 CG_DIR, strerror(errno));
		return;
	}
	rmdir(CG_DIR "/.probe");
	cg_ok = 1;
}

int cg_path(uint32_t i, const char *leaf, char *buf, size_t cap)
{
	//FUCKING UDEV BULLSHIT
	int n = snprintf(buf, cap, CG_DIR "/svc-%u%s%s", i, *leaf ? "/" : "", leaf);
	return n > 0 && (size_t)n < cap;
}

void cgroup_make(uint32_t i, char *procs, size_t cap)
{
	char dir[CG_PATH_MAX];

	procs[0] = '\0';
	if (!cg_ok)
		return;
	if (!cg_path(i, "", dir, sizeof(dir)) ||
	    !cg_path(i, "cgroup.procs", procs, cap)) {
		procs[0] = '\0';
		log_warn("%s: cgroup path is too long, using its process group", ng_name(map, i));
		return;
	}
	if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
		procs[0] = '\0';
		log_warn("%s: mkdir %s: %s, using its process group",
			 ng_name(map, i), dir, strerror(errno));
	}
}

int cgroup_populated(uint32_t i)
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

int cgroup_drop(uint32_t i)
{
	char dir[CG_PATH_MAX];

	if (!cg_ok || !cg_path(i, "", dir, sizeof(dir)))
		return 1;
	if (rmdir(dir) == 0 || errno == ENOENT)
		return 1;
	if (errno != EBUSY)
		log_warn("%s: rmdir %s: %s", ng_name(map, i), dir, strerror(errno));
	return 0;
}

int cgroup_signal(uint32_t i, int sig)
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
		if (ok)
			runs[i].cg_killed = 1;
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
