#include "init.h"
#include "util.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int same_dir(const char *a, const char *b)
{
	struct stat sa, sb;

	return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
	       sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

int graph_image(const char *path)
{
	struct ng_hdr h;
	struct stat st;
	ssize_t k;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return 0;
	if (fstat(fd, &st) < 0 || (uint64_t)st.st_size < sizeof(h)) {
		close(fd);
		return 0;
	}
	k = read(fd, &h, sizeof(h));
	close(fd);
	if (k != (ssize_t)sizeof(h))
		return 0;

	return h.magic == NG_MAGIC && h.total_len == (uint32_t)st.st_size;
}

// outside the service directory the scan does not see the output, so it is checked here
void refuse_nongraph(const char *path)
{
	struct stat st;

	if (stat(path, &st) < 0)
		return;
	if (!S_ISREG(st.st_mode) || !graph_image(path))
		die("%s: the output would replace this, and it is not a depgraph", path);
}

int lock_dir(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		die("open %s: %s", path, strerror(errno));
	if (flock(fd, LOCK_EX) < 0)
		die("lock %s: %s", path, strerror(errno));
	return fd;
}

static int same_fd_dir(int fd, const char *path)
{
	struct stat a, b;

	return fstat(fd, &a) == 0 && stat(path, &b) == 0 &&
	       a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

void write_atomic(const char *path, const void *buf, size_t len, mode_t mode, int held)
{
	char tmp[8224], old[4104], parent[4104], *slash;
	const char *dir, *base;
	const char *p = buf;
	size_t left = len;
	mode_t um;
	int fd, dfd, owned = 0;

	if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent))
		die("output path is too long: %s", path);

	slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		base = path + (slash - parent) + 1;
		dir = *parent ? parent : "/";
	} else {
		base = path;
		dir = ".";
	}

	if (snprintf(tmp, sizeof(tmp), "%s/.%s.XXXXXX", dir, base) >= (int)sizeof(tmp) ||
	    snprintf(old, sizeof(old), "%s.old", path) >= (int)sizeof(old))
		die("output path is too long: %s", path);

	// re-locking a directory this process already holds would deadlock
	if (held >= 0 && same_fd_dir(held, dir)) {
		dfd = held;
	} else {
		dfd = lock_dir(dir);
		owned = 1;
	}

	um = umask(0);
	umask(um);

	fd = mkostemp(tmp, O_CLOEXEC);
	if (fd < 0)
		die("mkstemp %s: %s", tmp, strerror(errno));
	if (fchmod(fd, mode & ~um) < 0) {
		unlink(tmp);
		die("fchmod %s: %s", tmp, strerror(errno));
	}

	while (left) {
		ssize_t n = write(fd, p, left);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			unlink(tmp);
			die("write %s: %s", tmp, strerror(errno));
		}
		p += n;
		left -= (size_t)n;
	}
	if (fsync(fd) < 0) {
		unlink(tmp);
		die("fsync %s: %s", tmp, strerror(errno));
	}
	close(fd);

	unlink(old);
	if (link(path, old) < 0 && errno != ENOENT) {
		unlink(tmp);
		die("link %s -> %s: %s", path, old, strerror(errno));
	}

	if (rename(tmp, path) < 0) {
		unlink(tmp);
		die("rename %s -> %s: %s", tmp, path, strerror(errno));
	}

	if (fsync(dfd) < 0)
		die("%s: written, but fsync %s failed: %s", path, dir, strerror(errno));
	if (owned)
		close(dfd);
}

// the basename of OUT when OUT is in DIR, the scan then knows the output and its .old by name
const char *output_base(const char *dir, const char *out, size_t *out_base_len)
{
	char outdir[4096];
	char *slash;
	const char *out_base;

	if (snprintf(outdir, sizeof(outdir), "%s", out) >= (int)sizeof(outdir))
		usage_die("init: output path is too long: %s", out);
	slash = strrchr(outdir, '/');
	if (slash) {
		out_base = out + (slash - outdir) + 1;
		if (slash == outdir)
			outdir[1] = '\0';
		else
			*slash = '\0';
	} else {
		out_base = out;
		outdir[0] = '.';
		outdir[1] = '\0';
	}
	if (!*out_base || !same_dir(dir, outdir))
		out_base = NULL;
	*out_base_len = out_base ? strlen(out_base) : 0;
	return out_base;
}
