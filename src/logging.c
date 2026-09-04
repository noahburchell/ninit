#include "logging.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int log_fd = 2;
static int log_color;
static struct timespec log_start;

static const char *const tag_color[] = {
	"\033[32m  OK  \033[0m",
	"      ",
	"\033[1;33m WARN \033[0m",
	"\033[1;31m FAIL \033[0m",
};

static const char *const tag_plain[] = {
	"  OK  ",
	"      ",
	" WARN ",
	" FAIL ",
};

void log_init(void)
{
	clock_gettime(CLOCK_MONOTONIC, &log_start);
	log_color = isatty(log_fd);
}

void log_reopen_console(void)
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (log_fd != 2)
		close(log_fd);
	log_fd = fd;
	log_color = isatty(log_fd);
}

static int stamp(char *buf, size_t cap)
{
	struct timespec now;
	long long ms;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (long long)(now.tv_sec - log_start.tv_sec) * 1000 +
	     (now.tv_nsec - log_start.tv_nsec) / 1000000;
	if (ms < 0)
		ms = 0;

	return snprintf(buf, cap, "[%4lld.%03lld] ", ms / 1000, ms % 1000);
}

void ninit_log(int level, const char *fmt, ...)
{
	char buf[1024];
	const char *const *tags = log_color ? tag_color : tag_plain;
	va_list ap;
	int n;

	if (level < LOG_OK || level > LOG_ERR)
		level = LOG_INFO;

	n = stamp(buf, sizeof(buf));
	n += snprintf(buf + n, sizeof(buf) - n, "%s ", tags[level]);

	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);

	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';

	// one write so concurrent reaping cannot interleave a line
	(void)!write(log_fd, buf, (size_t)n);
}

void log_raw(int level, const char *buf, size_t len)
{
	const char *p = buf, *end = buf + len;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);

		if (n)
			ninit_log(level, "         %.*s", (int)n, p);
		if (!nl)
			break;
		p = nl + 1;
	}
}

void print_welcome(void)
{
#ifdef NINIT_QUIET
	return;
#else
	FILE *file = fopen("/etc/os-release", "r");
	char line[128];
	int found = 0;

	if (!file)
		file = fopen("/usr/lib/os-release", "r");

	while (file && fgets(line, sizeof(line), file)) {
		char *name;
		size_t len;

		if (strncmp(line, "PRETTY_NAME=", 12))
			continue;

		name = line + 12;
		if (*name == '"')
			name++;
		len = strlen(name);
		if (len && name[len - 1] == '\n')
			name[--len] = '\0';
		if (len && name[len - 1] == '"')
			name[--len] = '\0';

		log_info("Welcome to %s!", name);
		found = 1;
		break;
	}
	if (file)
		fclose(file);
	if (!found)
		log_info("Welcome to Linux!");
#endif
}
