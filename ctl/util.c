#include "util.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/openat2.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

const char *g_dir;
const char *g_root;
const char *g_root_pfx = "";
static int root_fd = -1;
static char *root_real;

void die(const char *fmt, ...)
{
	va_list ap;

	fputs("ninitctl: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

void usage_die(const char *fmt, ...)
{
	va_list ap;

	fputs("ninitctl: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

void *xmalloc(size_t n)
{
	void *p = calloc(1, n ? n : 1);

	if (!p)
		die("out of memory");
	return p;
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);

	if (!q)
		die("out of memory");
	return q;
}

// paths the graph names are then looked up below DIR, as the system booted from it sees them
void set_root(const char *dir)
{
	size_t n = strlen(dir);
	char *pfx;

	root_fd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0)
		die("open %s: %s", dir, strerror(errno));
	root_real = realpath(dir, NULL);
	if (!root_real)
		die("realpath %s: %s", dir, strerror(errno));
	pfx = xstrndup(dir, n);
	while (n && pfx[n - 1] == '/')
		pfx[--n] = '\0';
	g_root = dir;
	g_root_pfx = pfx;
}

// absolute links and .. stay below the root, as they would on the system itself
int sys_open(const char *path, int flags)
{
	struct open_how how = { .flags = (uint64_t)(flags | O_CLOEXEC),
				.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS };
	long fd;

	if (!g_root)
		return open(path, flags | O_CLOEXEC);
	fd = syscall(SYS_openat2, root_fd, path, &how, sizeof(how));
	if (fd < 0 && errno == ENOSYS)
		die("--root: openat2: %s", strerror(errno));
	return (int)fd;
}

int sys_stat(const char *path, struct stat *st)
{
	int fd, rc;

	if (!g_root)
		return stat(path, st);
	fd = sys_open(path, O_PATH);
	if (fd < 0)
		return -1;
	rc = fstat(fd, st);
	close(fd);
	return rc;
}

// services run as root, which may execute a file with any execute bit set
int sys_exec_ok(const char *path, const struct stat *st)
{
	if (!g_root)
		return access(path, X_OK);
	if (st->st_mode & 0111)
		return 0;
	errno = EACCES;
	return -1;
}

// DIR with every link resolved, below the root through the link /proc keeps for a descriptor
char *sys_realpath(const char *dir)
{
	char link[32], buf[PATH_MAX];
	size_t rl;
	ssize_t n;
	int fd;

	if (!g_root)
		return realpath(dir, NULL);
	fd = sys_open(dir, O_PATH | O_DIRECTORY);
	if (fd < 0)
		return NULL;
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	n = readlink(link, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return NULL;
	buf[n] = '\0';
	rl = strlen(root_real);
	if (rl == 1)
		return xstrndup(buf, (size_t)n);
	if (strncmp(buf, root_real, rl) || (buf[rl] && buf[rl] != '/'))
		return NULL;
	return buf[rl] ? xstrndup(buf + rl, (size_t)n - rl) : xstrndup("/", 1);
}

void strv_push(struct strv *s, const char *v)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 4;
		s->v = xrealloc(s->v, s->cap * sizeof(*s->v));
	}
	s->v[s->n++] = v;
}

char *slurp(const char *path, size_t *len)
{
	struct stat st;
	char *buf;
	int fd;
	ssize_t got, pos = 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		die("open %s: %s", path, strerror(errno));
	if (fstat(fd, &st) < 0)
		die("stat %s: %s", path, strerror(errno));
	if ((uint64_t)st.st_size > NG_MAX_SRC)
		die("%s: is %llu bytes, the maximum is %u", path,
		    (unsigned long long)st.st_size, NG_MAX_SRC);

	buf = xmalloc((size_t)st.st_size + 1);
	while (pos < st.st_size) {
		got = read(fd, buf + pos, (size_t)st.st_size - pos);
		if (got < 0)
			die("read %s: %s", path, strerror(errno));
		if (!got)
			break;
		pos += got;
	}
	close(fd);

	buf[pos] = '\0';
	*len = (size_t)pos;
	return buf;
}

char *xstrndup(const char *s, size_t n)
{
	char *d = xmalloc(n + 1);

	memcpy(d, s, n);
	d[n] = '\0';
	return d;
}
