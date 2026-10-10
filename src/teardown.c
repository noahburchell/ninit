#include "teardown.h"
#include "fail.h"
#include "logging.h"
#include "ngraph.h"
#include "ninit.h"
#include "signals.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/swap.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define HWCLOCK_GRACE_MS 5000
#define RO_PASSES	3

static void unescape_mount(char *s)
{
	char *w = s;

	for (; *s; s++) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7' &&
		    s[3] >= '0' && s[3] <= '7') {
			*w++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0'));
			s += 3;
		} else {
			*w++ = *s;
		}
	}
	*w = '\0';
}

void remount_ro(void)
{
	char *buf = NULL, *p, **mps = NULL;
	size_t cap = 0, len = 0, n = 0, mcap = 0;
	int fd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC | O_NOCTTY);

	if (fd >= 0) {
		for (;;) {
			ssize_t k;

			if (len + 4096 > cap) {
				char *nb = realloc(buf, cap = cap ? cap * 2 : 16384);

				if (!nb) {
					log_warn("shutdown: out of memory reading the mount table");
					break;
				}
				buf = nb;
			}
			k = read(fd, buf + len, cap - len - 1);
			if (k < 0 && errno == EINTR)
				continue;
			if (k <= 0)
				break;
			len += (size_t)k;
		}
		close(fd);
	}

	if (buf) {
		buf[len] = '\0';
		for (p = buf; *p; ) {
			char *nl = strchr(p, '\n'), *sp, *mp;

			if (nl)
				*nl = '\0';
			sp = strchr(p, ' ');
			mp = sp ? sp + 1 : NULL;
			sp = mp ? strchr(mp, ' ') : NULL;
			if (mp && sp) {
				*sp = '\0';
				unescape_mount(mp);
				if (n == mcap) {
					char **nm = realloc(mps, (mcap = mcap ? mcap * 2 : 64) * sizeof(*mps));

					if (!nm) {
						log_warn("shutdown: out of memory listing mounts");
						break;
					}
					mps = nm;
				}
				mps[n++] = mp;
			}
			if (!nl)
				break;
			p = nl + 1;
		}
	}

	for (int pass = 0; pass < RO_PASSES; pass++) {
		size_t left = 0, k = n;

		while (k--) {
			if (!mps[k])
				continue;
			if (mount(NULL, mps[k], NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0)
				mps[k] = NULL;
			else
				left++;
		}
		if (!left)
			break;
	}

	size_t stuck = 0;

	for (size_t k = n; k--; ) {
		if (!mps[k] || !strcmp(mps[k], "/"))
			continue;
		if (umount2(mps[k], 0) == 0) {
			log_warn("shutdown: %s: remount read-only failed, unmounted", mps[k]);
			mps[k] = NULL;
			continue;
		}
		stuck++;
	}

	if (stuck) {
		sync();
		for (size_t k = n; k--; ) {
			if (!mps[k] || !strcmp(mps[k], "/"))
				continue;
			if (mount(NULL, mps[k], NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0) {
				log_warn("shutdown: %s: remounted read-only on retry", mps[k]);
				mps[k] = NULL;
				continue;
			}
			if (umount2(mps[k], 0) == 0) {
				log_warn("shutdown: %s: unmounted on retry", mps[k]);
				mps[k] = NULL;
				continue;
			}
			log_err("shutdown: %s: still mounted writable (%s), detaching",
				mps[k], strerror(errno));
			if (umount2(mps[k], MNT_DETACH) < 0)
				log_warn("shutdown: detaching %s: %s", mps[k], strerror(errno));
		}
	}

	for (int pass = 0; pass < RO_PASSES; pass++)
		if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0)
			break;
		else if (pass == RO_PASSES - 1)
			log_err("shutdown: /: remount read-only failed: %s", strerror(errno));

	free(mps);
	free(buf);
}

// the hardware clock and swap both need the filesystems still writable
void stop_swap(void)
{
	char line[512];
	FILE *f = fopen("/proc/swaps", "re");

	if (!f)
		return;
	if (!fgets(line, sizeof(line), f)) { // header
		fclose(f);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char *sp = strchr(line, ' ');

		if (!sp)
			sp = strchr(line, '\t');
		if (!sp)
			continue;
		*sp = '\0';
		unescape_mount(line);
		if (swapoff(line) < 0)
			log_warn("shutdown: swapoff %s: %s", line, strerror(errno));
	}
	fclose(f);
}

static int wait_pid_ms(pid_t pid, long long ms)
{
	long long deadline = now_ms() + ms;
	int st;

	for (;;) {
		struct pollfd p = { .fd = sfd, .events = POLLIN };
		struct signalfd_siginfo si;
		pid_t r = waitpid(pid, &st, WNOHANG);
		long long left;

		if (r == pid || (r < 0 && errno != EINTR))
			return 1;
		left = deadline - now_ms();
		if (left <= 0)
			return 0;
		if (left > SHUTDOWN_DRAIN_MS)
			left = SHUTDOWN_DRAIN_MS;
		poll(&p, 1, (int)left);
		while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si))
			;
	}
}

static int rtc_local(void)
{
	char buf[256], *p;
	ssize_t k;
	int fd = open("/etc/adjtime", O_RDONLY | O_CLOEXEC | O_NOCTTY);

	if (fd < 0)
		return 0;
	k = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (k <= 0)
		return 0;
	buf[k] = '\0';
	p = strchr(buf, '\n');
	if (p)
		p = strchr(p + 1, '\n');
	return p && !strncmp(p + 1, "LOCAL", 5) && (!p[6] || p[6] == '\n');
}

void save_hwclock(void)
{
	static char arg0[] = "hwclock", arg1[] = "--systohc", utc[] = "--utc", local[] = "--localtime";
	static char path[] = NG_PATH;
	static char *const envp[] = { path, NULL };
	char *const argv[] = { arg0, arg1, rtc_local() ? local : utc, NULL };
	pid_t pid = fork();

	if (pid < 0)
		return;
	if (pid == 0) {
		ninit_cloexec_except(-1);
		execve("/sbin/hwclock", argv, envp);
		execve("/usr/sbin/hwclock", argv, envp);
		_exit(127);
	}
	if (wait_pid_ms(pid, HWCLOCK_GRACE_MS))
		return;

	log_warn("shutdown: hwclock did not exit in %d s, killing it",
		 HWCLOCK_GRACE_MS / 1000);
	kill(pid, SIGKILL);
	wait_pid_ms(pid, KILL_GRACE_MS);
}
