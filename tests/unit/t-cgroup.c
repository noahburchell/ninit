#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tap.h"

// the cgroup files of pid 1, served from a directory the test fills

static char root[] = "/tmp/ninit-t-cgroup.XXXXXX";
static pid_t killed[16];
static int killed_sig[16], n_killed;

static const char* under_root(const char* path) {
	static char buf[512];
	size_t l = strlen("/sys/fs/cgroup/ninit.services");

	// every path pid 1 opens here is below its cgroup directory
	if(strncmp(path, "/sys/fs/cgroup/ninit.services", l)) {
		abort();
	}

	snprintf(buf, sizeof(buf), "%s%s", root, path + l);

	return buf;
}

static int shim_open(const char* path, int flags, ...) {
	return open(under_root(path), flags);
}

static int shim_rmdir(const char* path) {
	return rmdir(under_root(path));
}

// a real pid would reach a real process, so nothing is sent
static int shim_kill(pid_t pid, int sig) {
	if(n_killed < 16) {
		killed[n_killed] = pid;
		killed_sig[n_killed] = sig;
	}

	n_killed++;

	return 0;
}

#define open  shim_open
#define rmdir shim_rmdir
#define kill  shim_kill
#include "../../src/cgroup.c"
#undef open
#undef rmdir
#undef kill

#include "gbuild.h"
#include "logcap.h"

const void* map;
struct run* runs;

static void put(const char* rel, const char* text) {
	char path[512];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if(fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text)) {
		abort();
	}

	close(fd);
}

static void get(const char* rel, char* buf, size_t cap) {
	char path[512];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	n = fd >= 0 ? read(fd, buf, cap - 1) : -1;
	buf[n > 0 ? n : 0] = '\0';
	if(fd >= 0) {
		close(fd);
	}
}

static void rm(const char* rel) {
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	unlink(path);
}

static void sub(const char* rel) {
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	if(mkdir(path, 0755) < 0 && errno != EEXIST) {
		abort();
	}
}

static void test_signal(void) {
	char got[16];

	sub("svc-0");
	put("svc-0/cgroup.procs", "3000001\n1\n0\n3000002\n");
	put("svc-0/cgroup.kill", "");

	n_killed = 0;
	ok(cgroup_signal(0, SIGKILL) == 0, "SIGKILL goes through cgroup.kill");
	get("svc-0/cgroup.kill", got, sizeof(got));
	is_str(got, "1", "which is written 1");
	ok(!n_killed && runs[0].cg_killed, "no process is signalled one by one, and the cgroup is marked killed");

	rm("svc-0/cgroup.kill");
	runs[0].cg_killed = 0;
	n_killed = 0;
	ok(cgroup_signal(0, SIGKILL) == 2, "without cgroup.kill SIGKILL counts what it reached");
	ok(n_killed == 2 && killed[0] == 3000001 && killed[1] == 3000002
		   && killed_sig[0] == SIGKILL && killed_sig[1] == SIGKILL,
	   "and reaches every process listed in cgroup.procs, as before linux 5.14");
	ok(!runs[0].cg_killed, "the cgroup is not marked killed");

	n_killed = 0;
	ok(cgroup_signal(0, SIGTERM) == 2 && n_killed == 2 && killed_sig[0] == SIGTERM,
	   "any other signal reaches each listed process");

	n_killed = 0;
	ok(cgroup_signal(1, SIGKILL) < 0 && !n_killed, "a service without a cgroup directory is not signalled");

	cg_ok = 0;
	ok(cgroup_signal(0, SIGKILL) < 0 && !n_killed, "nor is anything without cgroups");
	cg_ok = 1;
}

static void test_populated(void) {
	sub("svc-0");
	put("svc-0/cgroup.events", "populated 1\nfrozen 0\n");
	is_int(cgroup_populated(0), 1, "a populated cgroup is reported");
	put("svc-0/cgroup.events", "populated 0\nfrozen 0\n");
	is_int(cgroup_populated(0), 0, "an empty one is not");
	put("svc-0/cgroup.events", "frozen 0\n");
	is_int(cgroup_populated(0), -1, "an events file without populated is unknown");
	is_int(cgroup_populated(1), -1, "and so is a missing cgroup");
}

static void test_drop(void) {
	char path[512];

	rm("svc-0/cgroup.events");
	rm("svc-0/cgroup.procs");
	is_int(cgroup_drop(0), 1, "an empty cgroup directory is removed");
	snprintf(path, sizeof(path), "%s/svc-0", root);
	ok(access(path, F_OK) < 0, "and is gone");
	is_int(cgroup_drop(0), 1, "a missing one counts as removed");
}

static void test_path(void) {
	char buf[CG_PATH_MAX];

	ok(cg_path(7, "cgroup.procs", buf, sizeof(buf))
		   && !strcmp(buf, "/sys/fs/cgroup/ninit.services/svc-7/cgroup.procs"),
	   "the path of a cgroup file names the index");
	ok(cg_path(7, "", buf, sizeof(buf)) && !strcmp(buf, "/sys/fs/cgroup/ninit.services/svc-7"),
	   "an empty leaf is the directory itself");
	ok(!cg_path(7, "cgroup.procs", buf, 20), "a path longer than the buffer is refused");
}

int main(void) {
	static const struct gb_svc sv[] = { GB_ONESHOT("a", ":"), GB_ONESHOT("b", ":") };
	struct gb_img g = gb_build(sv, 2, NULL, 0);

	logcap_quiet();
	map = g.map;
	runs = calloc(2, sizeof(*runs));
	if(!mkdtemp(root) || !runs) {
		abort();
	}

	cg_ok = 1;

	test_path();
	test_signal();
	test_populated();
	test_drop();

	rmdir(root);
	free(runs);
	free(g.map);

	return tap_done();
}
