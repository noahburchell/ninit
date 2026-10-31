#include "../help/str.h"

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

static const char* const act_name[] = {
	[ACT_REBOOT] = "reboot",
	[ACT_POWEROFF] = "poweroff",
	[ACT_HALT] = "halt",
};

static const int act_signal[] = {
	[ACT_REBOOT] = SIGTERM,
	[ACT_POWEROFF] = SIGUSR2,
	[ACT_HALT] = SIGUSR1,
};

static const unsigned act_rb[] = {
	[ACT_REBOOT] = RB_AUTOBOOT,
	[ACT_POWEROFF] = RB_POWER_OFF,
	[ACT_HALT] = RB_HALT_SYSTEM,
};

static void usage(const char* me, FILE* f) {
	fprintf(f,
		"usage: %s [OPTION]...%s\n"
		"\n"
		"  -r         reboot\n"
		"  -h, -P     power off\n"
		"  -H         halt without powering off\n"
		"  -f         reboot directly, without signalling pid 1\n"
		"  -c         report how to cancel a pending shutdown\n"
		"      --help display this help and exit\n"
		"\n"
		"TIME is now, +MINUTES or HH:MM\n"
		"reboot, poweroff and halt accept the same options\n",
		me, strcmp(path_base(me), "shutdown") && strcmp(path_base(me), "ninit-shutdown") ? "" : " TIME");
}

static int pid1_is_ninit(void) {
	char buf[64];
	ssize_t n;
	int fd = open("/proc/1/comm", O_RDONLY | O_CLOEXEC);

	if(fd < 0) {
		return -1;
	}

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if(n <= 0) {
		return -1;
	}

	buf[n] = '\0';
	buf[strcspn(buf, "\n")] = '\0';

	return !strcmp(buf, "ninit");
}

static void fallback(const char* name, char** argv) __attribute__((noreturn));

static void fallback(const char* name, char** argv) {
	char path[PATH_MAX + 128], arg0[64];
	char self[PATH_MAX];
	ssize_t n;

	n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if(n > 0) {
		char* slash;

		self[n] = '\0';
		slash = strrchr(self, '/');
		if(slash) {
			*slash = '\0';
		}

		snprintf(path, sizeof(path), "%s/%s.old", self, name);
	} else {
		snprintf(path, sizeof(path), "%s/%s.old", NINIT_SBINDIR, name);
	}

	snprintf(arg0, sizeof(arg0), "%s", name);
	argv[0] = arg0;
	// a setuid install must not lend its privilege to the other init's tool
	if(setgid(getgid()) < 0) {
		fprintf(stderr, "%s: setgid: %s\n", name, strerror(errno));
		exit(1);
	}

	if(setuid(getuid()) < 0) {
		fprintf(stderr, "%s: setuid: %s\n", name, strerror(errno));
		exit(1);
	}

	execv(path, argv);

	fprintf(stderr, "%s: pid 1 is not ninit and %s: %s\n",
		name, path, strerror(errno));
	exit(1);
}

// seconds until the moment described, or -1
static long parse_when(const char* s) {
	char* end;
	long v;

	if(!strcmp(s, "now")) {
		return 0;
	}

	if(*s == '+') {
		long secs;

		if(s[1] < '0' || s[1] > '9') {
			return -1;
		}

		errno = 0;
		v = strtol(s + 1, &end, 10);
		if(errno || end == s + 1 || *end || v < 0 || ckd_mul(&secs, v, 60L)) {
			return -1;
		}

		return secs;
	}

	if(strchr(s, ':')) {
		struct tm tm;
		time_t now = time(NULL);
		long hh, mm, delta;

		// strtol takes a missing number for 0 and skips blanks and signs
		if(*s < '0' || *s > '9') {
			return -1;
		}

		errno = 0;
		hh = strtol(s, &end, 10);
		if(errno || *end != ':' || hh > 23 || end[1] < '0' || end[1] > '9') {
			return -1;
		}

		mm = strtol(end + 1, &end, 10);
		if(errno || *end || mm > 59) {
			return -1;
		}

		if(!localtime_r(&now, &tm)) {
			return -1;
		}

		delta = (hh - tm.tm_hour) * 3600 + (mm - tm.tm_min) * 60 - tm.tm_sec;
		if(delta < 0) {
			delta += 24 * 3600;
		}

		return delta;
	}

	return -1;
}

static int try_logind(enum act a) {
	static const char* const method[] = {
		[ACT_REBOOT] = "org.freedesktop.login1.Manager.Reboot",
		[ACT_POWEROFF] = "org.freedesktop.login1.Manager.PowerOff",
		[ACT_HALT] = "org.freedesktop.login1.Manager.Halt",
	};
	pid_t pid, got;
	int st = -1;

	pid = fork();
	if(pid < 0) {
		return -1;
	}

	if(pid == 0) {
		execlp("dbus-send", "dbus-send", "--system",
		       "--dest=org.freedesktop.login1", "/org/freedesktop/login1",
		       method[a], "boolean:true", (char*)NULL);
		_exit(127);
	}

	while((got = waitpid(pid, &st, 0)) < 0 && errno == EINTR)
		;
	if(got != pid) {
		return -1;
	}

	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

// a user waits for TIME only after logind says the request can be made
static int logind_can(enum act a) {
	static const char* const method[] = {
		[ACT_REBOOT] = "org.freedesktop.login1.Manager.CanReboot",
		[ACT_POWEROFF] = "org.freedesktop.login1.Manager.CanPowerOff",
		[ACT_HALT] = "org.freedesktop.login1.Manager.CanHalt",
	};
	char buf[32];
	char* w;
	size_t len = 0;
	ssize_t n;
	pid_t pid, got;
	int p[2], st = -1;

	if(pipe2(p, O_CLOEXEC) < 0) {
		return 0;
	}

	pid = fork();
	if(pid < 0) {
		close(p[0]);
		close(p[1]);
		return 0;
	}

	if(pid == 0) {
		dup2(p[1], 1);
		execlp("dbus-send", "dbus-send", "--system", "--print-reply=literal",
		       "--dest=org.freedesktop.login1", "/org/freedesktop/login1",
		       method[a], (char*)NULL);
		_exit(127);
	}

	close(p[1]);
	while(len < sizeof(buf) - 1) {
		n = read(p[0], buf + len, sizeof(buf) - 1 - len);
		if(n < 0 && errno == EINTR) {
			continue;
		}

		if(n <= 0) {
			break;
		}

		len += (size_t)n;
	}

	close(p[0]);
	buf[len] = '\0';
	while((got = waitpid(pid, &st, 0)) < 0 && errno == EINTR)
		;
	if(got != pid || !WIFEXITED(st) || WEXITSTATUS(st)) {
		return 0;
	}

	w = buf + strspn(buf, " \t\n");
	w[strcspn(w, " \t\n")] = '\0';

	// challenge leaves the decision to polkit when the request is made
	return !strcmp(w, "yes") || !strcmp(w, "challenge");
}

static void warn_console(const char* me, enum act a, long secs) {
	char msg[160];
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	int n;

	if(fd < 0) {
		return;
	}

	if(secs >= 60) {
		n = snprintf(msg, sizeof(msg), "\r\n%s: %s in %ld minute%s\r\n",
			     me, act_name[a], secs / 60, plural((unsigned long)(secs / 60)));
	} else {
		n = snprintf(msg, sizeof(msg), "\r\n%s: %s in %ld second%s\r\n",
			     me, act_name[a], secs, plural((unsigned long)secs));
	}

	if(n >= (int)sizeof(msg)) {
		n = sizeof(msg) - 1;
	}

	if(n > 0) {
		(void)!write(fd, msg, (size_t)n);
	}

	close(fd);
}

int main(int argc, char** argv) {
	const char* me = argc > 0 && argv[0] ? path_base(argv[0]) : "ninit-shutdown";
	enum act act;
	long when = 0;
	int is_shutdown, is_telinit, force = 0, have_when = 0, privileged, k;

	// another init gets its own tool with every argument, whatever ninit would make of them
	if(pid1_is_ninit() == 0) {
		fallback(me, argv);
	}

	if(!strcmp(me, "reboot")) {
		act = ACT_REBOOT;
	} else if(!strcmp(me, "poweroff")) {
		act = ACT_POWEROFF;
	} else if(!strcmp(me, "halt")) {
		act = ACT_HALT;
	} else {
		act = ACT_POWEROFF;
	}

	is_shutdown = !strcmp(me, "shutdown") || !strcmp(me, "ninit-shutdown");
	is_telinit = !strcmp(me, "telinit");

	for(k = 1; k < argc; k++) {
		const char* a = argv[k];

		if(!strcmp(a, "--help")) {
			usage(me, stdout);
			return 0;
		} else if(!strcmp(a, "-r")) {
			act = ACT_REBOOT;
		} else if(!strcmp(a, "-h") || !strcmp(a, "-P")) {
			act = ACT_POWEROFF;
		} else if(!strcmp(a, "-H")) {
			act = ACT_HALT;
		} else if(!strcmp(a, "-p")) {
			act = ACT_POWEROFF;
		} else if(!strcmp(a, "-f")) {
			force = 1;
		} else if(!strcmp(a, "-c")) {
			fprintf(stderr, "%s: a pending shutdown runs in the "
					"foreground, interrupt it instead\n",
				me);
			return 1;
		} else if(!strcmp(a, "-t") || !strcmp(a, "-k") || !strcmp(a, "-n")
			  || !strcmp(a, "-w") || !strcmp(a, "-d")) {
			if(!strcmp(a, "-t") && k + 1 < argc) {
				k++;
			}
		} else if(a[0] == '-' && a[1]) {
			fprintf(stderr, "%s: unrecognized option '%s'\n", me, a);
			usage(me, stderr);
			return 2;
		} else if(is_telinit) {
			if(!strcmp(a, "0")) {
				act = ACT_POWEROFF;
			} else if(!strcmp(a, "6")) {
				act = ACT_REBOOT;
			} else if(!strcmp(a, "q") || !strcmp(a, "Q")) {
				return 0;
			} else {
				fprintf(stderr, "%s: unknown runlevel '%s', expected 0 or 6\n", me, a);
				return 2;
			}

			have_when = 1;
		} else if(!have_when) {
			when = parse_when(a);
			if(when < 0) {
				fprintf(stderr, "%s: invalid time '%s'\n", me, a);
				return 2;
			}

			have_when = 1;
		}
	}

	if(is_telinit && !have_when) {
		fprintf(stderr, "%s: missing runlevel operand\n", me);
		return 2;
	}

	if(is_shutdown && !have_when) {
		fprintf(stderr, "%s: missing time operand\n", me);
		usage(me, stderr);
		return 2;
	}

	if(force) {
		if(getuid() != 0) {
			fprintf(stderr, "%s: -f requires root\n", me);
			return 1;
		}

		sync();
		reboot((int)act_rb[act]);
		fprintf(stderr, "%s: reboot: %s\n", me, strerror(errno));
		return 1;
	}

	// signal 0 checks the permission alone, root, setuid root and cap_kill pass alike
	privileged = kill(1, 0) == 0;
	if(when > 0 && !privileged && !logind_can(act)) {
		fprintf(stderr, "%s: permission denied, must be run as root\n", me);
		return 1;
	}

	if(when > 0) {
		warn_console(me, act, when);
		printf("%s: %s in %ld seconds\n", me, act_name[act], when);
		fflush(stdout);
		// sleep takes an unsigned int and +MINUTES can ask for more
		while(when > 0) {
			unsigned s = when > UINT_MAX ? UINT_MAX : (unsigned)when;

			when -= (long)(s - sleep(s));
		}
	}

	if(!privileged) {
		if(try_logind(act) == 0) {
			return 0;
		}

		fprintf(stderr, "%s: permission denied, must be run as root\n", me);
		return 1;
	}

	if(kill(1, act_signal[act]) < 0) {
		fprintf(stderr, "%s: cannot signal pid 1: %s\n", me, strerror(errno));
		return 1;
	}

	return 0;
}
