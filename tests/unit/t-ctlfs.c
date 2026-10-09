#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tap.h"

// the file operations of ninitctl, with each call they make failing in turn

static jmp_buf die_jb;

[[noreturn]] static void shim_exit(int code)
{
	longjmp(die_jb, 1000 + code);
}

// the call that fails, by name, after fail_after calls succeed, fail_times times or -1 for ever
static const char *fail_call;
static int fail_after, fail_times, n_symlink;

static int failing(const char *call)
{
	if (!fail_call || strcmp(fail_call, call))
		return 0;
	if (fail_after > 0) {
		fail_after--;
		return 0;
	}
	if (!fail_times)
		return 0;
	if (fail_times > 0)
		fail_times--;
	return 1;
}

static void fail(const char *call, int after, int times)
{
	fail_call = call;
	fail_after = after;
	fail_times = times;
}

static int shim_mkostemp(char *tmpl, int flags)
{
	if (failing("mkostemp")) {
		errno = EACCES;
		return -1;
	}
	return mkostemp(tmpl, flags);
}

static int shim_fchmod(int fd, mode_t mode)
{
	if (failing("fchmod")) {
		errno = EPERM;
		return -1;
	}
	return fchmod(fd, mode);
}

static ssize_t shim_write(int fd, const void *buf, size_t n)
{
	if (failing("write")) {
		errno = ENOSPC;
		return -1;
	}
	// a short write, so the caller has to loop
	return write(fd, buf, n > 3 ? 3 : n);
}

static int shim_fsync(int fd)
{
	struct stat st;

	if (fstat(fd, &st) == 0 && failing(S_ISDIR(st.st_mode) ? "fsync dir" : "fsync")) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int shim_link(const char *a, const char *b)
{
	if (failing("link")) {
		errno = EPERM;
		return -1;
	}
	return link(a, b);
}

static int shim_rename(const char *a, const char *b)
{
	if (failing("rename")) {
		errno = EXDEV;
		return -1;
	}
	return rename(a, b);
}

static int shim_symlink(const char *a, const char *b)
{
	n_symlink++;
	if (failing("symlink")) {
		errno = EEXIST;
		return -1;
	}
	return symlink(a, b);
}

#define exit shim_exit
#include "../../ctl/util.c"
#undef exit
#define mkostemp shim_mkostemp
#define fchmod shim_fchmod
#define write shim_write
#define fsync shim_fsync
#define link shim_link
#define rename shim_rename
#define symlink shim_symlink
#include "../../ctl/output.c"
#include "../../ctl/add.c"
#undef mkostemp
#undef fchmod
#undef write
#undef fsync
#undef link
#undef rename
#undef symlink

static char errtext[4096];

// held is the locked directory, a die() would otherwise leave a lock behind that the next call waits on
struct atomic_arg {
	const char *path, *text;
	int held;
};

static void call_atomic(void *p)
{
	struct atomic_arg *a = p;

	write_atomic(a->path, a->text, strlen(a->text), 0644, a->held);
}

// runs fn with stderr captured, returns 0 or 1000 + the status die() exited with
static int capture(void (*fn)(void *), void *arg)
{
	char path[] = "/tmp/ninit-t-ctlfs-err.XXXXXX";
	int fd = mkstemp(path), saved;
	volatile int rc;
	ssize_t n;

	fflush(stderr);
	saved = dup(2);
	dup2(fd, 2);
	rc = setjmp(die_jb);
	if (!rc)
		fn(arg);
	fflush(stderr);
	dup2(saved, 2);
	close(saved);
	lseek(fd, 0, SEEK_SET);
	n = read(fd, errtext, sizeof(errtext) - 1);
	errtext[n > 0 ? n : 0] = '\0';
	close(fd);
	unlink(path);
	return rc;
}

static void slurp_to(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, buf, cap - 1) : -1;

	buf[n > 0 ? n : 0] = '\0';
	if (fd >= 0)
		close(fd);
}

// entries of DIR that begin with a dot, the temporary files write_atomic makes
static int dot_files(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	while (d && (e = readdir(d)))
		n += e->d_name[0] == '.' && strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
	if (d)
		closedir(d);
	return n;
}

static void test_atomic(void)
{
	// each message names the call and a path in the directory, the temporary one or the graph
	static const struct {
		const char *call, *msg, *path;
		int after;
	} v[] = {
		{ "mkostemp", "mkstemp", ".depgraph.", 0 },
		{ "fchmod", "fchmod", ".depgraph.", 0 },
		{ "write", "write", ".depgraph.", 1 },
		{ "fsync", "fsync", ".depgraph.", 0 },
		{ "link", "link", "depgraph -> ", 0 },
		{ "rename", "rename", ".depgraph.", 0 },
	};
	char dir[] = "/tmp/ninit-t-ctlfs.XXXXXX", path[256], old[256], want[1024], got[64];
	struct atomic_arg a = { path, "first", -1 };
	int rc;

	if (!mkdtemp(dir))
		abort();
	a.held = lock_dir(dir);
	snprintf(path, sizeof(path), "%s/depgraph", dir);
	snprintf(old, sizeof(old), "%s/depgraph.old", dir);

	rc = capture(call_atomic, &a);
	slurp_to(path, got, sizeof(got));
	ok(!rc && !strcmp(got, "first") && access(old, F_OK) < 0, "a first graph is written through short writes");
	a.text = "second";
	rc = capture(call_atomic, &a);
	slurp_to(path, got, sizeof(got));
	ok(!rc && !strcmp(got, "second"), "a second replaces it");
	slurp_to(old, got, sizeof(got));
	is_str(got, "first", "and the first is kept as .old");

	a.text = "third";
	for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		fail(v[k].call, v[k].after, -1);
		rc = capture(call_atomic, &a);
		fail(NULL, 0, 0);
		snprintf(want, sizeof(want), "ninitctl: %s %s/%s", v[k].msg, dir, v[k].path);
		if (!ok(rc == 1001 && strstr(errtext, want), "a failed %s ends the build", v[k].call))
			tap_diag("%d %s", rc, errtext);
		slurp_to(path, got, sizeof(got));
		ok(!strcmp(got, "second") && !dot_files(dir), "the graph is unchanged and no temporary file is left");
	}

	fail("fsync dir", 0, -1);
	rc = capture(call_atomic, &a);
	fail(NULL, 0, 0);
	snprintf(want, sizeof(want), "ninitctl: %s/depgraph: written, but fsync %s failed: Input/output error\n",
		 dir, dir);
	ok(rc == 1001 && !strcmp(errtext, want), "a directory that cannot be synced is reported after the write");
	slurp_to(path, got, sizeof(got));
	is_str(got, "third", "the graph is in place");

	close(a.held);
	unlink(path);
	unlink(old);
	rmdir(dir);
}

// the entries of DIR made by relink for a temporary link
static int tmp_links(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	while (d && (e = readdir(d)))
		n += !!strstr(e->d_name, ".ninitctl.");
	if (d)
		closedir(d);
	return n;
}

static void read_link(const char *path, char *buf, size_t cap)
{
	ssize_t n = readlink(path, buf, cap - 1);

	buf[n > 0 ? n : 0] = '\0';
}

static void test_relink(void)
{
	char dir[] = "/tmp/ninit-t-ctlfs-link.XXXXXX", path[256], got[256], longt[4300];

	if (!mkdtemp(dir))
		abort();
	snprintf(path, sizeof(path), "%s/svc", dir);
	if (symlink("../shared/svc", path) < 0)
		abort();

	ok(relink(path, "../shared/svc", 1), "a link moved up a directory is rewritten");
	read_link(path, got, sizeof(got));
	is_str(got, "shared/svc", "to point where it pointed before");

	fail("symlink", 0, 3);
	n_symlink = 0;
	ok(relink(path, "shared/svc", 0) && n_symlink == 4, "a temporary name that exists is skipped for the next");
	read_link(path, got, sizeof(got));
	is_str(got, "../shared/svc", "and the link is rewritten");

	fail("symlink", 0, -1);
	n_symlink = 0;
	ok(!relink(path, "../shared/svc", 1) && n_symlink == 1000, "after 1000 names that exist it gives up");
	read_link(path, got, sizeof(got));
	is_str(got, "../shared/svc", "and the link is unchanged");

	fail("rename", 0, -1);
	ok(!relink(path, "../shared/svc", 1), "a failed rename gives up");
	fail(NULL, 0, 0);
	ok(!tmp_links(dir), "and removes its temporary link");
	read_link(path, got, sizeof(got));
	is_str(got, "../shared/svc", "leaving the link unchanged");

	memset(longt, 'x', sizeof(longt) - 1);
	longt[sizeof(longt) - 1] = '\0';
	n_symlink = 0;
	ok(!relink(path, longt, 0) && !n_symlink, "a target too long to rewrite gives up before any link is made");

	unlink(path);
	rmdir(dir);
}

int main(void)
{
	test_atomic();
	test_relink();
	return tap_done();
}
