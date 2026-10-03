#include "boot.h"
#include "cgroup.h"
#include "logging.h"
#include "ngraph.h"
#include "ninit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHILD_NOFILE_CUR	1024
#define CHILD_NOFILE_MAX	524288

struct rlimit child_nofile;
char env_locale[NG_LOCALE_MAX][NG_LOCALE_LEN] = { "LANG=" NG_FALLBACK_LANG };
int n_locale = 1;

void ensure_stdio(void)
{
	int fd;

	for (fd = 0; fd < 3; fd++) {
		int f;

		if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
			continue;
		f = open("/dev/null", (fd ? O_WRONLY : O_RDONLY) | O_NOCTTY);
		if (f < 0)
			f = open("/", O_RDONLY);
		if (f < 0)
			continue;
		if (f != fd) {
			dup2(f, fd);
			close(f);
		}
	}
}

static int is_mountpoint(const char *path)
{
	struct stat a, b;
	char parent[64];

	if (stat(path, &a) < 0)
		return 0;
	if (snprintf(parent, sizeof(parent), "%s/..", path) >= (int)sizeof(parent))
		return 0;
	if (stat(parent, &b) < 0)
		return 0;
	return a.st_dev != b.st_dev;
}

static void mount_one(const char *src, const char *dst, const char *type,
		      unsigned long flags, const char *data)
{
	if (is_mountpoint(dst))
		return;
	if (mkdir(dst, 0755) < 0 && errno != EEXIST) {
		log_warn("mount: mkdir %s: %s", dst, strerror(errno));
		return;
	}
	if (mount(src, dst, type, flags, data) < 0 && errno != EBUSY)
		log_warn("mount: %s on %s: %s", type, dst, strerror(errno));
}

void mount_api_fs(void)
{
	mount_one("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount_one("sys", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount_one("dev", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mount_one("run", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
	mount_one("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "mode=0620,gid=5,ptmxmode=0666");
	mount_one("shm", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
	mount_one("cgroup2", CG_BASE, "cgroup2", MS_NOSUID | MS_NOEXEC | MS_NODEV, "nsdelegate");
}

// devtmpfs makes the device nodes but not these
void seed_dev(void)
{
	static const char *const link[][2] = {
		{ "/proc/self/fd",	"/dev/fd" },
		{ "/proc/self/fd/0",	"/dev/stdin" },
		{ "/proc/self/fd/1",	"/dev/stdout" },
		{ "/proc/self/fd/2",	"/dev/stderr" },
	};

	for (size_t i = 0; i < sizeof(link) / sizeof(*link); i++)
		if (symlink(link[i][0], link[i][1]) < 0 && errno != EEXIST)
			log_warn("dev: symlink %s: %s", link[i][1], strerror(errno));

	if (access("/proc/kcore", F_OK) == 0 &&
	    symlink("/proc/kcore", "/dev/core") < 0 && errno != EEXIST)
		log_warn("dev: symlink /dev/core: %s", strerror(errno));
}

void raise_nofile(void)
{
	rlim_t need = 2 * (rlim_t)NG_MAX_SVC + 256;
	rlim_t hi = CHILD_NOFILE_MAX > need ? CHILD_NOFILE_MAX : need;
	rlim_t lo = CHILD_NOFILE_MAX > need ? need : CHILD_NOFILE_MAX;
	rlim_t ask[3];
	struct rlimit rl, orig;
	int n = 0, k;

	if (getrlimit(RLIMIT_NOFILE, &orig) < 0) {
		orig.rlim_cur = 1024;
		orig.rlim_max = 4096;
	}
	// strictly descending
	if (hi > orig.rlim_max)
		ask[n++] = hi;
	if (lo < hi && lo > orig.rlim_max)
		ask[n++] = lo;
	ask[n++] = orig.rlim_max;

	rl = orig;
	if (rl.rlim_cur < need)
		rl.rlim_cur = need;
	for (k = 0; k < n; k++) {
		rl.rlim_max = ask[k];
		if (rl.rlim_cur > rl.rlim_max)
			rl.rlim_cur = rl.rlim_max;
		if (setrlimit(RLIMIT_NOFILE, &rl) == 0)
			break;
	}
	if (k == n) {
		log_warn("setrlimit(RLIMIT_NOFILE): %s", strerror(errno));
		rl = orig;
	}

	child_nofile.rlim_max = rl.rlim_max > CHILD_NOFILE_MAX ? CHILD_NOFILE_MAX : rl.rlim_max;
	child_nofile.rlim_cur = child_nofile.rlim_max < CHILD_NOFILE_CUR ?
				child_nofile.rlim_max : CHILD_NOFILE_CUR;
}

const void *load_graph(const char *path, const char **why)
{
	struct stat st;
	void *m;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (fd < 0) {
		*why = strerror(errno);
		return NULL;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
		close(fd);
		*why = "not a regular file";
		return NULL;
	}
	if ((uint64_t)st.st_size > UINT32_MAX) {
		close(fd);
		*why = "larger than 4 GiB";
		return NULL;
	}

	m = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) {
		close(fd);
		*why = strerror(errno);
		return NULL;
	}

	for (off_t got = 0; got < st.st_size; ) {
		ssize_t k = read(fd, (char *)m + got, (size_t)(st.st_size - got));

		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0) {
			close(fd);
			munmap(m, (size_t)st.st_size);
			*why = k < 0 ? strerror(errno) : "unexpected end of file";
			return NULL;
		}
		got += k;
	}
	close(fd);
	if (mprotect(m, (size_t)st.st_size, PROT_READ) < 0) {
		munmap(m, (size_t)st.st_size);
		*why = strerror(errno);
		return NULL;
	}

	*why = ng_verify(m, (size_t)st.st_size);
	if (!*why && !((const struct ng_hdr *)m)->n_svc)
		*why = "no services to start";
	if (*why) {
		munmap(m, (size_t)st.st_size);
		return NULL;
	}
	return m;
}

void load_locale(void)
{
	const char *why;
	int n = ng_locale_env(env_locale, NG_LOCALE_MAX, &why);

	if (why)
		log_warn("locale: %s %s, ignoring the rest", NG_LOCALE_CONF, why);
	if (n > 0) {
		n_locale = n;
		return;
	}
	snprintf(env_locale[0], NG_LOCALE_LEN, "LANG=%s", NG_FALLBACK_LANG);
	n_locale = 1;
}
