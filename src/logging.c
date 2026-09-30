#include "logging.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_WRITE_MS	100
#define LOG_BATCH_MS	250

static int log_fd = 2;
static int log_color;
static struct timespec log_start;
static unsigned long log_dropped;
static int log_stalled;
static long long log_wait_until;

static const char *const tag_color[] = {
	"\033[32mDONE\033[0m",
	"",
	"\033[33mWARN\033[0m",
	"\033[31mFAIL\033[0m",
	"\033[1;34mWAIT\033[0m",
	"\033[36mNOTE\033[0m",
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

static_assert(!(LOG_RING & (LOG_RING - 1)) && LOG_RING > LOG_LINE, "LOG_RING must be a power of two above LOG_LINE");

// every line in the ring ends in a newline. ring_tail is the start of the oldest one
static char log_ring[LOG_RING];
static uint64_t ring_head, ring_tail;
static void (*ring_feed)(void);

// the offset past the newline that ends the line starting at at, at < ring_head
static uint64_t ring_eol(uint64_t at)
{
	for (;;) {
		size_t off = (size_t)(at & (LOG_RING - 1)), seg = LOG_RING - off;
		const char *nl;

		if (seg > ring_head - at)
			seg = (size_t)(ring_head - at);
		nl = memchr(log_ring + off, '\n', seg);
		if (nl)
			return at + (uint64_t)(nl - (log_ring + off)) + 1;
		at += seg;
	}
}

static void ring_put(const char *s, size_t n)
{
	size_t off = (size_t)(ring_head & (LOG_RING - 1)), k = LOG_RING - off;

	while (ring_head + n - ring_tail > LOG_RING)
		ring_tail = ring_eol(ring_tail);
	if (k > n)
		k = n;
	memcpy(log_ring + off, s, k);
	memcpy(log_ring, s + k, n - k);
	ring_head += n;
	if (ring_feed)
		ring_feed();
}

// fn runs after every append, to serve readers the ring would otherwise overtake
void log_feed(void (*fn)(void))
{
	ring_feed = fn;
}

uint64_t log_first(void)
{
	return ring_tail;
}

uint64_t log_end(void)
{
	return ring_head;
}

static long long log_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_set_nonblock(void)
{
	int fl = fcntl(log_fd, F_GETFL);

	if (fl >= 0 && !(fl & O_NONBLOCK))
		fcntl(log_fd, F_SETFL, fl | O_NONBLOCK);
}

void log_batch_begin(void)
{
	log_wait_until = log_now_ms() + LOG_BATCH_MS;
	log_stalled = 0;
}

void log_init(void)
{
	clock_gettime(CLOCK_MONOTONIC, &log_start);
	log_color = isatty(log_fd);
	log_set_nonblock();
	log_batch_begin();
}

void log_reopen_console(void)
{
	int fd = open("/dev/console", O_WRONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (log_fd != 2)
		close(log_fd);
	log_fd = fd;
	log_color = isatty(log_fd);
	log_set_nonblock();
}

void log_adopt_fd(int fd)
{
	log_fd = fd;
	log_color = isatty(fd);
	log_set_nonblock();
}

static int log_write(const char *buf, size_t len)
{
	long long deadline = 0, now;

	while (len) {
		struct pollfd p = { .fd = log_fd, .events = POLLOUT };
		ssize_t w = write(log_fd, buf, len);

		if (w > 0) {
			buf += w;
			len -= (size_t)w;
			log_stalled = 0;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && errno != EAGAIN) {
			log_dropped++;
			return 0;
		}
		now = log_now_ms();
		if (log_stalled || now >= log_wait_until) {
			log_dropped++;
			return 0;
		}
		if (!deadline) {
			deadline = now + LOG_WRITE_MS;
			if (deadline > log_wait_until)
				deadline = log_wait_until;
		} else if (now >= deadline) {
			log_stalled = 1;
			log_dropped++;
			return 0;
		}
		poll(&p, 1, 10);
	}

	return 1;
}

#define LOG_PREFIX_MAX	64

static int fitted(int ret, int at, size_t cap)
{
	if (ret < 0)
		return at;
	if ((size_t)ret >= cap - (size_t)at)
		return (int)cap - 1;
	return at + ret;
}

static long long log_elapsed_ms(void)
{
	struct timespec now;
	long long ms;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (long long)(now.tv_sec - log_start.tv_sec) * 1000 +
	     (now.tv_nsec - log_start.tv_nsec) / 1000000;
	return ms < 0 ? 0 : ms;
}

static int prefix(char *buf, size_t cap, const char *tag, long long ms)
{
	int n = fitted(snprintf(buf, cap, "[%03lld.%03lld] %s > ", ms / 1000, ms % 1000, tag),
		       0, cap);

	return n > LOG_PREFIX_MAX ? LOG_PREFIX_MAX : n;
}

// copies the line at *at into buf, which holds LOG_LINE bytes, and advances *at.
// a reader the ring has overtaken gets a notice instead. returns 0 at the end
size_t log_line(uint64_t *at, char *buf)
{
	uint64_t end;
	size_t off, n, k;
	int p;

	if (*at < ring_tail) {
		p = prefix(buf, LOG_LINE, tag_plain[LOG_WARN], log_elapsed_ms());
		p = fitted(snprintf(buf + p, LOG_LINE - (size_t)p, "log: skipped %llu bytes\n",
				    (unsigned long long)(ring_tail - *at)), p, LOG_LINE);
		*at = ring_tail;
		return (size_t)p;
	}
	if (*at >= ring_head)
		return 0;
	end = ring_eol(*at);
	n = (size_t)(end - *at);
	off = (size_t)(*at & (LOG_RING - 1));
	k = LOG_RING - off < n ? LOG_RING - off : n;
	memcpy(buf, log_ring + off, k);
	memcpy(buf + k, log_ring, n - k);
	*at = end;
	return n;
}

static int log_report_dropped(unsigned long lost)
{
	const char *const *tags = log_color ? tag_color : tag_plain;
	char buf[160];
	int n, ret;

	n = prefix(buf, sizeof(buf), tags[LOG_WARN], log_elapsed_ms());
	ret = snprintf(buf + n, sizeof(buf) - n, "console: dropped %lu message%s",
		       lost, lost == 1 ? "" : "s");
	n = fitted(ret, n, sizeof(buf));
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';

	return log_write(buf, (size_t)n);
}

void ninit_log(int level, const char *fmt, ...)
{
	char buf[LOG_LINE], con[LOG_LINE + 16];
	int nocon = level & LOG_NOCON, n, p = 0, c, ret;
	long long ms = log_elapsed_ms();
	va_list ap;

	level &= ~LOG_NOCON;
	if ((unsigned)level >= LOG_N)
		level = LOG_INFO;
	if (level != LOG_INFO)
		p = prefix(buf, sizeof(buf), tag_plain[level], ms);

	va_start(ap, fmt);
	ret = vsnprintf(buf + p, sizeof(buf) - p, fmt, ap);
	va_end(ap);

	n = fitted(ret, p, sizeof(buf));
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';
	ring_put(buf, (size_t)n);
	if (nocon)
		return;

	if (log_dropped) {
		unsigned long lost = log_dropped;

		if (log_report_dropped(lost))
			log_dropped -= lost;
	}
	if (!log_color || !p) {
		log_write(buf, (size_t)n);
		return;
	}
	c = prefix(con, sizeof(con), tag_color[level], ms);
	memcpy(con + c, buf + p, (size_t)(n - p));
	log_write(con, (size_t)(c + n - p));
}

#define LOG_CONT	"         "

void log_raw(int level, const char *buf, size_t len)
{
	const char *p = buf, *end = buf + len;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);

		if (n)
			ninit_log(level, "%.*s", (int)n, p);
		if (!nl)
			break;
		p = nl + 1;
	}
}

void print_welcome(void)
{
	FILE *file = fopen("/etc/os-release", "r");
	char line[128], buf[LOG_LINE];
	const char *os = "Linux";
	int n;

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
		os = name;
		break;
	}
	if (file)
		fclose(file);

	// the ring gets it without the bold
	n = fitted(snprintf(buf, sizeof(buf), "\n" LOG_CONT "Welcome to %s!\n\n", os), 0, sizeof(buf));
	ring_put(buf, (size_t)n);
#ifndef NINIT_QUIET
	if (log_color)
		n = fitted(snprintf(buf, sizeof(buf), "\n" LOG_CONT "Welcome to \033[1m%s\033[0m!\n\n", os),
			   0, sizeof(buf));
	log_write(buf, (size_t)n);
#endif
}
