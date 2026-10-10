// serves one control connection with a canned reply, for testing the ninitctl client

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char buf[65536];
	int lfd, fd, rfd, logfd;
	size_t at = 0;
	ssize_t n;

	if (argc != 5) {
		fputs("usage: fakectl SOCK REPLY REQUEST-LOG READY-FILE\n", stderr);
		return 2;
	}
	lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[1]);
	unlink(argv[1]);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(lfd, 1) < 0) {
		fprintf(stderr, "fakectl: bind %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	close(open(argv[4], O_WRONLY | O_CREAT | O_CLOEXEC, 0644));
	fd = accept(lfd, NULL, NULL);
	if (fd < 0)
		return 1;
	while (at < sizeof(buf) && (n = read(fd, buf + at, sizeof(buf) - at)) > 0) {
		at += (size_t)n;
		if (memchr(buf, '\n', at))
			break;
	}
	logfd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	(void)!write(logfd, buf, at);
	close(logfd);
	rfd = open(argv[2], O_RDONLY | O_CLOEXEC);
	while (rfd >= 0 && (n = read(rfd, buf, sizeof(buf))) > 0)
		(void)!write(fd, buf, (size_t)n);
	close(fd);
	unlink(argv[1]);
	return 0;
}
