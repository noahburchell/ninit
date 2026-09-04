#include "logging.h"

#include <assert.h>
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
	"\033[32mDONE\033[0m",
	"",
	"\033[1;33mWARN\033[0m",
	"\033[1;31mFAIL\033[0m",
	"\033[1;34mWAIT\033[0m",
	"\033[1;36mNOTE\033[0m",
};

static const char *const tag_plain[] = {
	"DONE",
	"",
	"WARN",
	"FAIL",
	"WAIT",
	"NOTE",
};

static_assert(sizeof(tag_color) / sizeof(*tag_color) == LOG_N, "tag_color must cover every level");
static_assert(sizeof(tag_plain) / sizeof(*tag_plain) == LOG_N, "tag_plain must cover every level");

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

void log_adopt_fd(int fd)
{
	log_fd = fd;
	log_color = isatty(fd);
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

	if ((unsigned)level >= LOG_N)
		level = LOG_INFO;

	if (level == LOG_INFO) {
		n = 0;
	} else {
		n = stamp(buf, sizeof(buf));
		n += snprintf(buf + n, sizeof(buf) - n, "%s > ", tags[level]);
	}

	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);

	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';

	// one write so concurrent reaping cannot interleave a line
	(void)!write(log_fd, buf, (size_t)n);
}

#define LOG_CONT	"         "

void log_raw(int level, const char *buf, size_t len)
{
	const char *p = buf, *end = buf + len;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);

		if (n)
			ninit_log(level, LOG_CONT "%.*s", (int)n, p);
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
		char quote;

		if (strncmp(line, "PRETTY_NAME=", 12))
			continue;

		name = line + 12;
		quote = (*name == '"' || *name == '\'') ? *name++ : 0;
		len = strlen(name);
		if (len && name[len - 1] == '\n')
			name[--len] = '\0';
		if (quote && len && name[len - 1] == quote)
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
