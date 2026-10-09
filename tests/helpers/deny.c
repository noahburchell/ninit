#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// runs PROG with the named system calls failing with ENOSYS, as on a kernel that lacks them

#if defined(__x86_64__)
#define DENY_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define DENY_ARCH AUDIT_ARCH_AARCH64
#else
#error "no audit arch for this target"
#endif

static const struct {
	const char *name;
	long nr;
} calls[] = {
#ifdef SYS_clone3
	{ "clone3", SYS_clone3 },
#endif
#ifdef SYS_signalfd
	{ "signalfd", SYS_signalfd },
#endif
	{ "signalfd4", SYS_signalfd4 },
	{ "openat2", SYS_openat2 },
};

static void usage(void)
{
	fputs("usage: deny CALL... -- PROG [ARG]...\n"
	      "\n"
	      "CALL is clone3, signalfd, signalfd4 or openat2\n",
	      stderr);
	_exit(2);
}

int main(int argc, char **argv)
{
	struct sock_filter f[64];
	struct sock_fprog prog = { .filter = f };
	int k, n = 0, at;

	for (at = 1; at < argc && strcmp(argv[at], "--"); at++)
		;
	if (at >= argc - 1 || at == 1 || at > 28)
		usage();

	f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
	f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DENY_ARCH, 1, 0);
	f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
	for (k = 1; k < at; k++) {
		size_t c;

		for (c = 0; c < sizeof(calls) / sizeof(*calls); c++)
			if (!strcmp(argv[k], calls[c].name))
				break;
		if (c == sizeof(calls) / sizeof(*calls)) {
			fprintf(stderr, "deny: unknown call '%s'\n", argv[k]);
			usage();
		}
		f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)calls[c].nr, 0, 1);
		f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
	}
	f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	prog.len = (unsigned short)n;

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
		fprintf(stderr, "deny: prctl: %s\n", strerror(errno));
		return 1;
	}
	execv(argv[at + 1], argv + at + 1);
	fprintf(stderr, "deny: exec %s: %s\n", argv[at + 1], strerror(errno));
	return 127;
}
