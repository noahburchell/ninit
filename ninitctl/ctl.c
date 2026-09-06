#include "ctl.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
			fprintf(stderr, "ninitctl: pid 1 is not ninit\n");
		else if (errno == EACCES)
			fprintf(stderr, "ninitctl: only root may control services\n");
		close(fd);
		return -1;
	}
	return fd;
}

int cmd_ctl(const char *verb, int argc, char **argv)
{
	char line[1024];
	const char *name = NULL;
	int fd, k, rc = 0;
	ssize_t n;
	size_t at = 0;

	for (k = 0; k < argc; k++) {
		if (!strcmp(argv[k], "--")) {
			if (++k < argc)
				name = argv[k];
			continue;
		}
		if (argv[k][0] == '-' && argv[k][1]) {
			fprintf(stderr, "ninitctl: %s: unknown option '%s'\n", verb, argv[k]);
			return 2;
		}
		if (name) {
			fprintf(stderr, "ninitctl: %s takes at most one service name\n", verb);
			return 2;
		}
		name = argv[k];
	}

	if (!name && (!strcmp(verb, "start") || !strcmp(verb, "stop") ||
		      !strcmp(verb, "restart"))) {
		fprintf(stderr, "ninitctl: %s needs a service name\n", verb);
		return 2;
	}
	if (name && (!strcmp(verb, "resume") || !strcmp(verb, "reload"))) {
		fprintf(stderr, "ninitctl: %s takes no arguments\n", verb);
		return 2;
	}

	fd = ctl_connect();
	if (fd < 0)
		return 1;

	at = (size_t)snprintf(line, sizeof(line), "%s%s%s\n", verb, name ? " " : "",
			      name ? name : "");
	if (at >= sizeof(line)) {
		fprintf(stderr, "ninitctl: %s: name is too long\n", verb);
		close(fd);
		return 2;
	}
	if (write(fd, line, at) != (ssize_t)at) {
		fprintf(stderr, "ninitctl: write: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	while ((n = read(fd, line, sizeof(line) - 1)) > 0) {
		line[n] = '\0';
		if (strstr(line, "err ") == line || strstr(line, "\nerr "))
			rc = 1;
		fputs(line, stdout);
	}
	if (n < 0) {
		fprintf(stderr, "ninitctl: read: %s\n", strerror(errno));
		rc = 1;
	}
	close(fd);
	return rc;
}
