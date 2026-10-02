#pragma once

// reads back what was logged into the ring since logcap_begin()

#include "logging.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static uint64_t logcap_mark;

static inline void logcap_quiet(void)
{
	int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

	if (fd >= 0)
		log_adopt_fd(fd);
}

static inline void logcap_begin(void)
{
	logcap_mark = log_end();
}

static inline const char *logcap_text(void)
{
	static char buf[1 << 18];
	uint64_t p = logcap_mark;
	size_t at = 0;

	while (p < log_end() && at + LOG_LINE + 1 < sizeof(buf))
		at += log_line(&p, buf + at);
	buf[at] = '\0';
	return buf;
}

static inline unsigned logcap_count(const char *needle)
{
	const char *p = logcap_text();
	unsigned n = 0;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}
