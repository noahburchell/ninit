#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tap.h"

// the graph loader and the descriptor limit of pid 1, against files the test writes

static int mmap_fail, mprotect_fail, read_fail, read_eof, read_eintr;

static void *shim_mmap(void *a, size_t n, int prot, int flags, int fd, off_t off)
{
	if (mmap_fail) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return mmap(a, n, prot, flags, fd, off);
}

static int shim_mprotect(void *a, size_t n, int prot)
{
	if (mprotect_fail) {
		errno = EACCES;
		return -1;
	}
	return mprotect(a, n, prot);
}

static ssize_t shim_read(int fd, void *buf, size_t n)
{
	if (read_eintr) {
		read_eintr--;
		errno = EINTR;
		return -1;
	}
	if (read_fail) {
		errno = EIO;
		return -1;
	}
	if (read_eof)
		return 0;
	// one byte at a time, so the loader has to loop
	return read(fd, buf, n ? 1 : 0);
}

static struct rlimit lim_got = { 1024, 4096 }, lim_set;
static int getrlimit_fail, n_setrlimit;
static rlim_t setrlimit_max;

static int shim_getrlimit(int res, struct rlimit *rl)
{
	if (res != RLIMIT_NOFILE)
		abort();
	if (getrlimit_fail) {
		errno = EPERM;
		return -1;
	}
	*rl = lim_got;
	return 0;
}

// a hard limit above setrlimit_max is refused, as for a process without CAP_SYS_RESOURCE
static int shim_setrlimit(int res, const struct rlimit *rl)
{
	if (res != RLIMIT_NOFILE || rl->rlim_cur > rl->rlim_max)
		abort();
	n_setrlimit++;
	if (rl->rlim_max > setrlimit_max) {
		errno = EPERM;
		return -1;
	}
	lim_set = *rl;
	return 0;
}

[[noreturn]] static void shim_forbidden(void)
{
	abort();
}

static int shim_mount(const char *a, const char *b, const char *c, unsigned long d, const void *e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	shim_forbidden();
}

static int shim_symlink(const char *a, const char *b)
{
	(void)a;
	(void)b;
	shim_forbidden();
}

static int shim_mkdir(const char *a, mode_t b)
{
	(void)a;
	(void)b;
	shim_forbidden();
}

#define mmap shim_mmap
#define mprotect shim_mprotect
#define read shim_read
#define getrlimit shim_getrlimit
#define setrlimit shim_setrlimit
#define mount shim_mount
#define symlink shim_symlink
#define mkdir shim_mkdir
// the forbidden shims make callers such as mount_one noreturn here, never in ninit
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsuggest-attribute=noreturn"
#endif
#include "../../src/boot.c"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#undef mmap
#undef mprotect
#undef read
#undef getrlimit
#undef setrlimit
#undef mount
#undef symlink
#undef mkdir

#include "gbuild.h"
#include "logcap.h"

static char dir[] = "/tmp/ninit-t-boot.XXXXXX";

static const char *put_graph(const char *name, const void *buf, size_t len)
{
	static char path[256];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || write(fd, buf, len) != (ssize_t)len)
		abort();
	close(fd);
	return path;
}

static void test_load_graph(void)
{
	static const struct gb_svc sv[] = { GB_ONESHOT("a", ":") };
	struct gb_img g = gb_build(sv, 1, NULL, 0), none = gb_build(NULL, 0, NULL, 0);
	const char *why, *p;
	const void *m;
	char sub[300];
	int fd;

	if (!mkdtemp(dir))
		abort();

	p = put_graph("depgraph", g.map, g.len);
	m = load_graph(p, &why);
	ok(m && !why && !memcmp(m, g.map, g.len), "a valid graph loads, read one byte at a time");
	if (m)
		munmap((void *)(uintptr_t)m, g.len);

	read_eintr = 3;
	m = load_graph(p, &why);
	ok(m && !why, "an interrupted read is retried");
	if (m)
		munmap((void *)(uintptr_t)m, g.len);
	read_eintr = 0;

	snprintf(sub, sizeof(sub), "%s/missing", dir);
	m = load_graph(sub, &why);
	ok(!m && why && !strcmp(why, "No such file or directory"), "a missing graph names the error");
	m = load_graph(dir, &why);
	ok(!m && why && !strcmp(why, "not a regular file"), "a directory is not a graph");
	p = put_graph("empty", "", 0);
	m = load_graph(p, &why);
	ok(!m && why && !strcmp(why, "not a regular file"), "nor is an empty file");

	snprintf(sub, sizeof(sub), "%s/huge", dir);
	fd = open(sub, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd >= 0 && ftruncate(fd, (off_t)UINT32_MAX + 1) == 0) {
		close(fd);
		m = load_graph(sub, &why);
		ok(!m && why && !strcmp(why, "larger than 4 GiB"), "a file over 4 GiB is refused before it is read");
	} else {
		if (fd >= 0)
			close(fd);
		ok(1, "# SKIP a sparse file of 4 GiB cannot be made in %s", dir);
	}
	unlink(sub);

	p = put_graph("depgraph", g.map, g.len);
	mmap_fail = 1;
	m = load_graph(p, &why);
	ok(!m && why && !strcmp(why, "Cannot allocate memory"), "a failed mapping names the error");
	mmap_fail = 0;
	read_fail = 1;
	m = load_graph(p, &why);
	ok(!m && why && !strcmp(why, "Input/output error"), "so does a failed read");
	read_fail = 0;
	read_eof = 1;
	m = load_graph(p, &why);
	ok(!m && why && !strcmp(why, "unexpected end of file"), "a file that ends early is refused");
	read_eof = 0;
	mprotect_fail = 1;
	m = load_graph(p, &why);
	ok(!m && why && !strcmp(why, "Permission denied"), "a mapping that cannot be made read-only is refused");
	mprotect_fail = 0;

	((char *)g.map)[0] ^= 1;
	p = put_graph("depgraph", g.map, g.len);
	m = load_graph(p, &why);
	ok(!m && why && !strcmp(why, "bad magic"), "a graph that fails verification gives the reason");

	p = put_graph("depgraph", none.map, none.len);
	m = load_graph(p, &why);
	ok(!m && why && !strcmp(why, "no services to start"), "a graph of no services is refused");

	unlink(p);
	snprintf(sub, sizeof(sub), "%s/empty", dir);
	unlink(sub);
	rmdir(dir);
	free(g.map);
	free(none.map);
}

static void test_nofile(void)
{
	rlim_t need = 2 * (rlim_t)NG_MAX_SVC + 256;

	lim_got = (struct rlimit){ 1024, 4096 };
	setrlimit_max = RLIM_INFINITY;
	n_setrlimit = 0;
	raise_nofile();
	ok(lim_set.rlim_cur == need && lim_set.rlim_max == CHILD_NOFILE_MAX && n_setrlimit == 1,
	   "with the capability the hard limit is raised to %u at once", CHILD_NOFILE_MAX);
	ok(child_nofile.rlim_cur == CHILD_NOFILE_CUR && child_nofile.rlim_max == CHILD_NOFILE_MAX,
	   "and services get %u of %u", CHILD_NOFILE_CUR, CHILD_NOFILE_MAX);

	setrlimit_max = need;
	n_setrlimit = 0;
	raise_nofile();
	ok(lim_set.rlim_cur == need && lim_set.rlim_max == need && n_setrlimit == 2,
	   "a refused hard limit falls back to what pid 1 needs");
	ok(child_nofile.rlim_max == need, "and services get that as their hard limit");

	setrlimit_max = 0;
	n_setrlimit = 0;
	logcap_begin();
	raise_nofile();
	ok(n_setrlimit == 3 && logcap_count("WARN > setrlimit(RLIMIT_NOFILE): Operation not permitted") == 1,
	   "when every limit is refused the failure is logged");
	ok(child_nofile.rlim_cur == 1024 && child_nofile.rlim_max == 4096, "and services keep the limits pid 1 had");

	getrlimit_fail = 1;
	setrlimit_max = RLIM_INFINITY;
	raise_nofile();
	ok(lim_set.rlim_max == CHILD_NOFILE_MAX, "an unreadable limit is taken as 1024 of 4096 and raised");
	getrlimit_fail = 0;

	lim_got = (struct rlimit){ 1024, 1048576 };
	n_setrlimit = 0;
	raise_nofile();
	ok(lim_set.rlim_max == 1048576 && n_setrlimit == 1 && child_nofile.rlim_max == CHILD_NOFILE_MAX,
	   "a hard limit already above %u is kept, and services are capped at it", CHILD_NOFILE_MAX);
}

int main(void)
{
	logcap_quiet();
	test_load_graph();
	test_nofile();
	return tap_done();
}
