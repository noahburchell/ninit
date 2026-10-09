#include "util.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

const char *g_dir;

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
