#pragma once

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

// N bytes, fewer at the end of the file, -1 on an error
static inline ssize_t read_full(int fd, void *buf, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t k = read(fd, (char *)buf + got, n - got);

		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0)
			return -1;
		if (!k)
			break;
		got += (size_t)k;
	}
	return (ssize_t)got;
}

static inline int write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n) {
		ssize_t k = write(fd, p, n);

		if (k > 0) {
			p += k;
			n -= (size_t)k;
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}
