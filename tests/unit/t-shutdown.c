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
static const char *swaps_path, *adjtime_path;

static int shim_open(const char *path, int flags, ...)
{
	const char *to = NULL;

	if (!strcmp(path, "/proc/swaps"))
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

static int shim_umount2(const char *a, int b)
{
	(void)a;
	(void)b;
	shim_forbidden();
}

static void shim_sync(void)
{
	shim_forbidden();
}

static pid_t shim_fork(void)
{
	shim_forbidden();
}

#define open shim_open
#define swapoff shim_swapoff
#define mount shim_mount
#define umount2 shim_umount2
#define sync shim_sync
#define fork shim_fork
// the forbidden shims make callers such as remount_ro noreturn here, never in ninit
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsuggest-attribute=noreturn"
#endif
#include "../../src/shutdown.c"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#undef open
#undef swapoff
#undef mount
#undef umount2
#undef sync
#undef fork

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

int main(void)
{
	logcap_quiet();
	test_swap();
	test_adjtime();
	return tap_done();
}
