#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#ifdef HAVE_STDCKDINT_H
#include <stdckdint.h>
#else
#define ckd_mul(r, a, b) __builtin_mul_overflow((a), (b), (r))
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef NINIT_SBINDIR
#define NINIT_SBINDIR "/sbin"
#endif

enum act {
	ACT_REBOOT,
	ACT_POWEROFF,
	ACT_HALT,
};

static const char *const act_name[] = {
	[ACT_REBOOT] = "reboot",
	[ACT_POWEROFF] = "poweroff",
	[ACT_HALT] = "halt",
};

static int act_signal(enum act a)
{
	switch (a) {
	case ACT_POWEROFF:	return SIGUSR2;
	case ACT_HALT:		return SIGUSR1;
	case ACT_REBOOT:
	default:		return SIGTERM;
	}
}

static int act_rb(enum act a)
{
	switch (a) {
	case ACT_POWEROFF:	return RB_POWER_OFF;
	case ACT_HALT:		return RB_HALT_SYSTEM;
	case ACT_REBOOT:
	default:		return RB_AUTOBOOT;
	}
}

static const char *base(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

static void usage(const char *me, FILE *f)
{
	fprintf(f,
		"Usage: %s [OPTION]...%s\n"
		"\n"
		"  -r         reboot\n"
		"  -h, -P     power off\n"
		"  -H         halt without powering off\n"
		"  -f         reboot directly, without signalling pid 1\n"
		"  -c         report how to cancel a pending shutdown\n"
		"      --help display this help and exit\n"
		"\n"
		"TIME is now, +MINUTES or HH:MM.\n"
		"reboot, poweroff and halt accept the same options.\n",
		me, strcmp(base(me), "shutdown") ? "" : " TIME");
}

static int pid1_is_ninit(void)
{
	char buf[64];
	ssize_t n;
	int fd = open("/proc/1/comm", O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	buf[strcspn(buf, "\n")] = '\0';

	return !strcmp(buf, "ninit");
}

static void fallback(const char *name, char **argv) __attribute__((noreturn));

static void fallback(const char *name, char **argv)
{
	char path[PATH_MAX + 128], arg0[64];
	char self[PATH_MAX];
	ssize_t n;

	n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n > 0) {
		char *slash;

		self[n] = '\0';
		slash = strrchr(self, '/');
		if (slash)
			*slash = '\0';
		snprintf(path, sizeof(path), "%s/%s.old", self, name);
	} else {
		snprintf(path, sizeof(path), "%s/%s.old", NINIT_SBINDIR, name);
	}

	snprintf(arg0, sizeof(arg0), "%s", name);
	argv[0] = arg0;
	execv(path, argv);

	fprintf(stderr, "%s: pid 1 is not ninit and %s: %s\n",
		name, path, strerror(errno));
	exit(1);
}

// seconds until the moment described, or -1
static long parse_when(const char *s)
{
	char *end;
	long v;

	if (!strcmp(s, "now"))
		return 0;

	if (*s == '+') {
		long secs;

		errno = 0;
		v = strtol(s + 1, &end, 10);
		if (errno || end == s + 1 || *end || v < 0 || ckd_mul(&secs, v, 60L))
			return -1;
		return secs;
	}

	if (strchr(s, ':')) {
		struct tm tm;
		time_t now = time(NULL);
		long hh, mm, delta;

		errno = 0;
		hh = strtol(s, &end, 10);
		if (errno || *end != ':' || hh < 0 || hh > 23)
			return -1;
		mm = strtol(end + 1, &end, 10);
		if (errno || *end || mm < 0 || mm > 59)
			return -1;

		if (!localtime_r(&now, &tm))
			return -1;
		delta = (hh - tm.tm_hour) * 3600 + (mm - tm.tm_min) * 60 - tm.tm_sec;
		if (delta < 0)
			delta += 24 * 3600;
		return delta;
	}

	return -1;
}

static int try_logind(enum act a)
{
	static const char *const method[] = {
		[ACT_REBOOT] = "org.freedesktop.login1.Manager.Reboot",
		[ACT_POWEROFF] = "org.freedesktop.login1.Manager.PowerOff",
		[ACT_HALT] = "org.freedesktop.login1.Manager.Halt",
	};
	pid_t pid, got;
	int st = -1;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execlp("dbus-send", "dbus-send", "--system",
		       "--dest=org.freedesktop.login1", "/org/freedesktop/login1",
		       method[a], "boolean:true", (char *)NULL);
		_exit(127);
	}
	while ((got = waitpid(pid, &st, 0)) < 0 && errno == EINTR)
		;
	if (got != pid)
		return -1;

	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static void warn_console(enum act a, long secs)
{
	char msg[160];
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	int n;

	if (fd < 0)
		return;
	if (secs >= 60)
		n = snprintf(msg, sizeof(msg),
			     "\r\nThe system is going down for %s in %ld minute%s\r\n",
			     act_name[a], secs / 60, secs / 60 == 1 ? "" : "s");
	else
		n = snprintf(msg, sizeof(msg),
			     "\r\nThe system is going down for %s in %ld second%s\r\n",
			     act_name[a], secs, secs == 1 ? "" : "s");
	if (n > 0)
		(void)!write(fd, msg, (size_t)n);
	close(fd);
}

int main(int argc, char **argv)
{
	const char *me = argc > 0 && argv[0] ? base(argv[0]) : "ninit-shutdown";
	enum act act;
	long when = 0;
	int is_shutdown, is_telinit, force = 0, have_when = 0, k;

	if (!strcmp(me, "reboot"))
		act = ACT_REBOOT;
	else if (!strcmp(me, "poweroff"))
		act = ACT_POWEROFF;
	else if (!strcmp(me, "halt"))
		act = ACT_HALT;
	else
		act = ACT_POWEROFF;

	is_shutdown = !strcmp(me, "shutdown") || !strcmp(me, "ninit-shutdown");
	is_telinit = !strcmp(me, "telinit");

	for (k = 1; k < argc; k++) {
		const char *a = argv[k];

		if (!strcmp(a, "--help")) {
			usage(me, stdout);
			return 0;
		} else if (!strcmp(a, "-r")) {
			act = ACT_REBOOT;
		} else if (!strcmp(a, "-h") || !strcmp(a, "-P")) {
			act = ACT_POWEROFF;
		} else if (!strcmp(a, "-H")) {
			act = ACT_HALT;
		} else if (!strcmp(a, "-p")) {
			act = ACT_POWEROFF;
		} else if (!strcmp(a, "-f")) {
			force = 1;
		} else if (!strcmp(a, "-c")) {
			fprintf(stderr, "%s: a pending shutdown runs in the "
				"foreground, interrupt it instead\n", me);
			return 1;
		} else if (!strcmp(a, "-t") || !strcmp(a, "-k") || !strcmp(a, "-n") ||
			   !strcmp(a, "-w") || !strcmp(a, "-d")) {
			if (!strcmp(a, "-t") && k + 1 < argc)
				k++;
		} else if (a[0] == '-' && a[1]) {
			fprintf(stderr, "%s: unrecognized option '%s'\n", me, a);
			usage(me, stderr);
			return 1;
		} else if (is_telinit) {
			if (!strcmp(a, "0")) {
				act = ACT_POWEROFF;
			} else if (!strcmp(a, "6")) {
				act = ACT_REBOOT;
			} else if (!strcmp(a, "q") || !strcmp(a, "Q")) {
				return 0;
			} else {
				fprintf(stderr, "%s: unknown runlevel '%s', expected 0 or 6\n", me, a);
				return 1;
			}
			have_when = 1;
		} else if (!have_when) {
			when = parse_when(a);
			if (when < 0) {
				fprintf(stderr, "%s: invalid time '%s'\n", me, a);
				return 1;
			}
			have_when = 1;
		}
	}

	if (is_telinit && !have_when) {
		fprintf(stderr, "%s: missing runlevel operand\n", me);
		return 1;
	}

	if (is_shutdown && !have_when) {
		fprintf(stderr, "%s: missing time operand\n", me);
		usage(me, stderr);
		return 1;
	}

	if (pid1_is_ninit() == 0)
		fallback(me, argv);

	if (force) {
		sync();
		reboot(act_rb(act));
		fprintf(stderr, "%s: reboot: %s\n", me, strerror(errno));
		return 1;
	}

	if (when > 0) {
		warn_console(act, when);
		printf("%s: %s in %ld seconds\n", me, act_name[act], when);
		fflush(stdout);
		while (when > 0)
			when = (long)sleep((unsigned)when);
	}

	if (geteuid() != 0) {
		if (try_logind(act) == 0)
			return 0;
		fprintf(stderr, "%s: permission denied, must be run as root\n", me);
		return 1;
	}

	if (kill(1, act_signal(act)) < 0) {
		fprintf(stderr, "%s: cannot signal pid 1: %s\n", me, strerror(errno));
		return 1;
	}

	return 0;
}
