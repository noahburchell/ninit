// raw client for the ninit control socket, used inside the qemu guests

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static const char *sock_path = "/run/ninit/control";

static int dial(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "nctl-raw: connect %s: %s\n", sock_path, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// \n \r \t \\ and \xNN
static size_t unescape(const char *s, char *out)
{
	size_t n = 0;

	for (; *s; s++) {
		if (*s != '\\' || !s[1]) {
			out[n++] = *s;
			continue;
		}
		s++;
		switch (*s) {
		case 'n':
			out[n++] = '\n';
			break;
		case 'r':
			out[n++] = '\r';
			break;
		case 't':
			out[n++] = '\t';
			break;
		case 'x': {
			char hex[3] = { s[1], s[1] ? s[2] : 0, 0 };

			out[n++] = (char)strtol(hex, NULL, 16);
			s += 2;
			break;
		}
		default:
			out[n++] = *s;
		}
	}
	return n;
}

static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t k = write(fd, p, n);

		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -1;
		p += k;
		n -= (size_t)k;
	}
	return 0;
}

static void usage(void)
{
	fputs("Usage: nctl-raw [-s SOCK] [-e] [-H N] [-q] [-p MS] [-t MS] [-l N] REQUEST\n"
	      "\n"
	      "  -s SOCK  control socket path\n"
	      "  -e       REQUEST is sent as given, with escapes and no added newline\n"
	      "  -H N     hold N idle connections open while REQUEST runs\n"
	      "  -q       close right after sending\n"
	      "  -p MS    wait MS before reading the reply\n"
	      "  -t MS    stop reading after MS without data\n"
	      "  -l N     stop after N reply lines\n"
	      "\n"
	      "Exit status is 0 for a + line, 1 for a - line, 3 for no final line, 4 on timeout.\n",
	      stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	int esc = 0, hold = 0, quit = 0, pause_ms = 0, idle_ms = -1, max_lines = -1, opt;
	int held[64], fd, rc = 3, lines = 0;
	char *req, buf[65536], last[4096];
	size_t rlen, ll = 0;
	long long idle_since;

	while ((opt = getopt(argc, argv, "s:eH:qp:t:l:")) != -1) {
		switch (opt) {
		case 's':
			sock_path = optarg;
			break;
		case 'e':
			esc = 1;
			break;
		case 'H':
			hold = atoi(optarg);
			break;
		case 'q':
			quit = 1;
			break;
		case 'p':
			pause_ms = atoi(optarg);
			break;
		case 't':
			idle_ms = atoi(optarg);
			break;
		case 'l':
			max_lines = atoi(optarg);
			break;
		default:
			usage();
		}
	}
	if (optind != argc - 1 || hold < 0 || hold > 64)
		usage();

	req = malloc(strlen(argv[optind]) + 2);
	if (esc) {
		rlen = unescape(argv[optind], req);
	} else {
		rlen = strlen(argv[optind]);
		memcpy(req, argv[optind], rlen);
		req[rlen++] = '\n';
	}

	for (int k = 0; k < hold; k++) {
		held[k] = dial();
		if (held[k] < 0)
			return 5;
	}
	fd = dial();
	if (fd < 0)
		return 5;
	if (rlen && write_all(fd, req, rlen) < 0) {
		fprintf(stderr, "nctl-raw: write: %s\n", strerror(errno));
		return 5;
	}
	if (quit)
		return 0;
	if (pause_ms)
		poll(NULL, 0, pause_ms);

	idle_since = now_ms();
	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int wait = -1;
		ssize_t n;

		if (idle_ms >= 0) {
			long long left = idle_since + idle_ms - now_ms();

			if (left <= 0) {
				rc = 4;
				break;
			}
			wait = (int)left;
		}
		if (poll(&p, 1, wait) == 0)
			continue;
		n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		idle_since = now_ms();
		fwrite(buf, 1, (size_t)n, stdout);
		for (ssize_t k = 0; k < n; k++) {
			if (buf[k] != '\n') {
				if (ll < sizeof(last) - 1)
					last[ll++] = buf[k];
				continue;
			}
			last[ll] = '\0';
			if (last[0] == '+' && last[1] == ' ')
				rc = 0;
			else if (last[0] == '-' && last[1] == ' ')
				rc = 1;
			ll = 0;
			lines++;
		}
		if (max_lines >= 0 && lines >= max_lines)
			break;
	}
	fflush(stdout);
	for (int k = 0; k < hold; k++)
		close(held[k]);
	return rc;
}
