#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tap.h"

// the shutdown steps run against files the test writes, nothing reaches the system

static long long fake_ms = 100000;
static const char *mounts_path, *swaps_path, *adjtime_path;

static int shim_open(const char *path, int flags, ...)
{
	const char *to = NULL;

	if (!strcmp(path, "/proc/self/mounts"))
		to = mounts_path;
	else if (!strcmp(path, "/proc/swaps"))
		to = swaps_path;
	else if (!strcmp(path, "/etc/adjtime"))
		to = adjtime_path;
	else
		abort();
	if (!to) {
		errno = ENOENT;
		return -1;
	}
	return open(to, flags);
}

static char swapped[8192];
static size_t n_swapped;
static const char *swap_busy;

// each name swapoff receives, followed by a newline
static int shim_swapoff(const char *path)
{
	size_t n = strlen(path);

	if (n_swapped + n + 1 < sizeof(swapped)) {
		memcpy(swapped + n_swapped, path, n);
		n_swapped += n;
		swapped[n_swapped++] = '\n';
		swapped[n_swapped] = '\0';
	}
	if (swap_busy && !strcmp(path, swap_busy)) {
		errno = EBUSY;
		return -1;
	}
	return 0;
}

struct fake_mnt {
	const char *path;
	// remounts refused before one succeeds, -1 refuses every one
	int ro_fails;
	int umount_ok;
	int ro, gone, detached;
};

static struct fake_mnt mnts[8];
static int n_mnts, n_sync, n_remount;

static struct fake_mnt *mnt_of(const char *path)
{
	for (int k = 0; k < n_mnts; k++)
		if (!strcmp(mnts[k].path, path))
			return &mnts[k];
	abort();
}

static int shim_mount(const char *src, const char *dst, const char *type, unsigned long flags,
		      const void *data)
{
	struct fake_mnt *m = mnt_of(dst);

	if (src || type || data || flags != (MS_REMOUNT | MS_RDONLY))
		abort();
	n_remount++;
	if (m->ro_fails) {
		if (m->ro_fails > 0)
			m->ro_fails--;
		errno = EBUSY;
		return -1;
	}
	m->ro = 1;
	return 0;
}

static int shim_umount2(const char *path, int flags)
{
	struct fake_mnt *m = mnt_of(path);

	if (flags == MNT_DETACH) {
		m->detached = 1;
		return 0;
	}
	if (flags)
		abort();
	if (!m->umount_ok) {
		errno = EBUSY;
		return -1;
	}
	m->gone = 1;
	return 0;
}

static void shim_sync(void)
{
	n_sync++;
}

// hwclock never runs, its pid only answers waitpid and kill
static const pid_t hw_pid = 4200000;
static int hw_exits, hw_killed, hw_forks, fork_fail;

static pid_t shim_fork(void)
{
	if (fork_fail) {
		errno = EAGAIN;
		return -1;
	}
	hw_forks++;
	return hw_pid;
}

static pid_t shim_waitpid(pid_t pid, int *st, int flags)
{
	if (pid != hw_pid || flags != WNOHANG)
		abort();
	if (!hw_exits && !hw_killed)
		return 0;
	*st = 0;
	return pid;
}

static int shim_kill(pid_t pid, int sig)
{
	if (pid != hw_pid || sig != SIGKILL)
		abort();
	hw_killed = 1;
	return 0;
}

static int shim_poll(struct pollfd *p, nfds_t n, int ms)
{
	(void)p;
	(void)n;
	fake_ms += ms;
	return 0;
}

static int shim_execve(const char *p, char *const *a, char *const *e)
{
	(void)p;
	(void)a;
	(void)e;
	abort();
}

#define open shim_open
#define swapoff shim_swapoff
#define mount shim_mount
#define umount2 shim_umount2
#define sync shim_sync
#define fork shim_fork
#define waitpid shim_waitpid
#define kill shim_kill
#define poll shim_poll
#define execve shim_execve
#include "../../src/shutdown.c"
#undef open
#undef swapoff
#undef mount
#undef umount2
#undef sync
#undef fork
#undef waitpid
#undef kill
#undef poll
#undef execve

#include "logcap.h"

int sfd = -1;

long long now_ms(void)
{
	return fake_ms;
}

void ninit_cloexec_except(int keep)
{
	(void)keep;
}

static void put_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
		abort();
	close(fd);
}

static void test_swap(void)
{
	char path[] = "/tmp/ninit-t-shutdown-swaps.XXXXXX", text[2048], expect[2048], longp[700];
	int fd = mkstemp(path);

	close(fd);
	swaps_path = path;

	put_file(path, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n"
		       "/dev/sda2                               partition\t8388604\t\t0\t\t-2\n"
		       "/swap\\040file                           file\t\t1048572\t\t0\t\t-3\n"
		       "/dev/zram0                              partition\t4194300\t\t0\t\t100");
	n_swapped = 0;
	swapped[0] = '\0';
	stop_swap();
	is_str(swapped, "/dev/sda2\n/swap file\n/dev/zram0\n",
	       "every swap area is turned off, escapes undone, the last line without a newline");

	// longer than the 511 bytes fgets once read, which split it into two names
	memset(longp, 'x', sizeof(longp) - 1);
	longp[0] = '/';
	longp[sizeof(longp) - 1] = '\0';
	snprintf(text, sizeof(text), "Filename Type Size Used Priority\n%s file 4 0 -2\n/dev/sdb1 partition 4 0 -3\n",
		 longp);
	put_file(path, text);
	n_swapped = 0;
	swapped[0] = '\0';
	swap_busy = "/dev/sdb1";
	logcap_begin();
	stop_swap();
	snprintf(expect, sizeof(expect), "%s\n/dev/sdb1\n", longp);
	is_str(swapped, expect, "a path of 699 bytes reaches swapoff whole");
	ok(logcap_count("WARN > shutdown: swapoff /dev/sdb1: Device or resource busy") == 1,
	   "a swapoff failure is a warning");
	swap_busy = NULL;

	put_file(path, "Filename Type Size Used Priority\n\nnoseparator\n");
	n_swapped = 0;
	swapped[0] = '\0';
	stop_swap();
	is_str(swapped, "", "an empty line and a line without a separator are skipped");

	put_file(path, "");
	stop_swap();
	is_str(swapped, "", "an empty /proc/swaps turns nothing off");
	unlink(path);
	swaps_path = NULL;
	stop_swap();
	is_str(swapped, "", "neither does a missing one");
}

static void test_adjtime(void)
{
	char path[] = "/tmp/ninit-t-shutdown-adjtime.XXXXXX", text[1024];
	int fd = mkstemp(path);

	close(fd);
	adjtime_path = path;
	// a drift written with %f can run to hundreds of digits
	memset(text, '9', 600);
	snprintf(text + 600, sizeof(text) - 600, ".0 0 0.0\n0\nLOCAL\n");
	put_file(path, text);
	ok(rtc_local() == 1, "LOCAL is found after a first line of 600 bytes");
	unlink(path);
	adjtime_path = NULL;
}

static void add_mnt(const char *path, int ro_fails, int umount_ok)
{
	mnts[n_mnts++] = (struct fake_mnt){ .path = path, .ro_fails = ro_fails, .umount_ok = umount_ok };
}

static void test_remount(void)
{
	char path[] = "/tmp/ninit-t-shutdown-mounts.XXXXXX";
	int fd = mkstemp(path);

	close(fd);
	mounts_path = path;
	put_file(path, "/dev/sda1 / ext4 rw 0 0\n"
		       "proc /proc proc rw 0 0\n"
		       "/dev/sda3 /home ext4 rw 0 0\n"
		       "/dev/sda4 /var ext4 rw 0 0\n"
		       "/dev/sda5 /mnt/a\\040b ext4 rw 0 0\n"
		       "/dev/sda6 /data ext4 rw 0 0\n");
	n_mnts = n_sync = n_remount = 0;
	add_mnt("/", -1, 0);
	add_mnt("/proc", 0, 0);
	add_mnt("/home", 2, 0);
	add_mnt("/var", -1, 1);
	add_mnt("/mnt/a b", 3, 0);
	add_mnt("/data", -1, 0);
	logcap_begin();
	remount_ro();
	ok(mnts[1].ro && mnts[2].ro, "a busy mount is read-only by the third pass");
	ok(mnts[3].gone && logcap_count("WARN > shutdown: /var: remount read-only failed, unmounted") == 1,
	   "one that stays writable is unmounted, with a warning");
	ok(mnts[4].ro && n_sync == 1 &&
	   logcap_count("WARN > shutdown: /mnt/a b: remounted read-only on retry") == 1,
	   "one that can be neither is remounted once more after a sync, its escape undone");
	ok(mnts[5].detached &&
	   logcap_count("FAIL > shutdown: /data: still mounted writable (Device or resource busy), detaching") == 1,
	   "one that fails that too is detached and reported");
	ok(!mnts[0].ro && logcap_count("FAIL > shutdown: /: remount read-only failed: Device or resource busy") == 1,
	   "a root that stays writable is reported");
	ok(!mnts[0].gone && !mnts[0].detached, "and never unmounted");

	n_mnts = n_sync = n_remount = 0;
	add_mnt("/", 0, 0);
	add_mnt("/proc", 0, 0);
	add_mnt("/home", 0, 0);
	add_mnt("/var", 0, 0);
	add_mnt("/mnt/a b", 0, 0);
	add_mnt("/data", 0, 0);
	logcap_begin();
	remount_ro();
	ok(n_remount == 7 && !n_sync && !*logcap_text(), "a clean remount takes one pass, then / once more, quietly");

	unlink(path);
	mounts_path = NULL;
	n_mnts = n_remount = 0;
	add_mnt("/", 0, 0);
	remount_ro();
	ok(n_remount == 1 && mnts[0].ro, "without a mount table / is still remounted");
}

static void test_hwclock(void)
{
	long long t0;

	hw_exits = 1;
	hw_killed = hw_forks = 0;
	logcap_begin();
	t0 = fake_ms;
	save_hwclock();
	ok(hw_forks == 1 && !hw_killed && fake_ms == t0 && !*logcap_text(), "an hwclock that exits is waited for");

	hw_exits = 0;
	logcap_begin();
	t0 = fake_ms;
	save_hwclock();
	ok(hw_killed && fake_ms - t0 >= 5000 && fake_ms - t0 < 5000 + SHUTDOWN_DRAIN_MS,
	   "one still running after 5 s is killed");
	ok(logcap_count("WARN > shutdown: hwclock did not exit in 5 s, killing it") == 1, "with a warning");

	fork_fail = 1;
	hw_forks = 0;
	save_hwclock();
	ok(!hw_forks, "a failed fork skips the clock");
	fork_fail = 0;
}

int main(void)
{
	logcap_quiet();
	test_swap();
	test_adjtime();
	test_remount();
	test_hwclock();
	return tap_done();
}
