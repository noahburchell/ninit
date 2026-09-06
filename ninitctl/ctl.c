#include "ctl.h"
#include "../src/nctl.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CTL_LINE 4096

static int ctl_connect(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0) {
		fprintf(stderr, "ninitctl: socket: %s\n", strerror(errno));
		return -1;
	}
	memcpy(sa.sun_path, NINIT_CTL_SOCK, sizeof(NINIT_CTL_SOCK));
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "ninitctl: %s: %s\n", NINIT_CTL_SOCK, strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr, "ninitctl: pid 1 is not ninit, or it is too old to listen\n");
		else if (errno == EACCES)
			fprintf(stderr, "ninitctl: only root may control services\n");
		close(fd);
		return -1;
	}
	return fd;
}

static int write_all(int fd, const char *p, size_t n)
{
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

int cmd_ctl(const char *verb, int argc, char **argv)
{
	char buf[CTL_LINE * 2], line[CTL_LINE];
	const char *name = NULL;
	size_t held = 0, at;
	int fd, k, endopts = 0, rc = NCTL_EXIT_USAGE, seen_end = 0;
	ssize_t n;

	for (k = 0; k < argc; k++) {
		if (!endopts && !strcmp(argv[k], "--")) {
			endopts = 1;
			continue;
		}
		if (!endopts && argv[k][0] == '-' && argv[k][1]) {
			fprintf(stderr, "ninitctl: %s: unknown option '%s'\n", verb, argv[k]);
			return 2;
		}
		if (name) {
			fprintf(stderr, "ninitctl: %s takes at most one service name\n", verb);
			return 2;
		}
		name = argv[k];
	}

	if (!name && strcmp(verb, "status") && strcmp(verb, "resume") &&
	    strcmp(verb, "reload") && strcmp(verb, "log")) {
		fprintf(stderr, "ninitctl: %s needs a service name\n", verb);
		return NCTL_EXIT_USAGE;
	}
	if (name && (!strcmp(verb, "resume") || !strcmp(verb, "reload") ||
		     !strcmp(verb, "log"))) {
		fprintf(stderr, "ninitctl: %s takes no arguments\n", verb);
		return NCTL_EXIT_USAGE;
	}

	at = (size_t)snprintf(line, sizeof(line), "%s%s%s\n", verb, name ? " " : "",
			      name ? name : "");
	if (at >= sizeof(line)) {
		fprintf(stderr, "ninitctl: %s: name is too long\n", verb);
		return NCTL_EXIT_USAGE;
	}

	fd = ctl_connect();
	if (fd < 0)
		return NCTL_EXIT_FAIL;
	if (write_all(fd, line, at) < 0) {
		fprintf(stderr, "ninitctl: write: %s\n", strerror(errno));
		close(fd);
		return NCTL_EXIT_FAIL;
	}

	while ((n = read(fd, buf + held, sizeof(buf) - held - 1)) > 0) {
		char *p = buf, *nl;

		held += (size_t)n;
		buf[held] = '\0';
		while ((nl = memchr(p, '\n', held - (size_t)(p - buf))) != NULL) {
			*nl = '\0';
			if (NCTL_IS_DATA(p)) {
				printf("%s\n", p + NCTL_TAG_LEN);
			} else if (NCTL_IS_OK(p)) {
				printf("%s\n", p + NCTL_TAG_LEN);
				rc = NCTL_EXIT_OK;
				seen_end = 1;
			} else if (NCTL_IS_ERR(p)) {
				fprintf(stderr, "ninitctl: %s\n", p + NCTL_TAG_LEN);
				rc = NCTL_EXIT_FAIL;
				seen_end = 1;
			} else if (*p) {
				fprintf(stderr, "ninitctl: unframed reply: %s\n", p);
			}
			p = nl + 1;
		}
		held -= (size_t)(p - buf);
		memmove(buf, p, held);
		if (held >= sizeof(buf) - 1) {
			fprintf(stderr, "ninitctl: reply record too long\n");
			close(fd);
			return 1;
		}
	}
	if (n < 0) {
		fprintf(stderr, "ninitctl: read: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	if (!seen_end) {
		fprintf(stderr, "ninitctl: %s: pid 1 closed the connection without a result; "
			"the operation may still be in progress\n", verb);
		return 1;
	}
	return rc;
}
