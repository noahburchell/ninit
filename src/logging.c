#include "logging.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define LOG_WRITE_MS	100
#define LOG_BATCH_MS	250
#define LOG_CON		(LOG_LINE + 16)

static int log_fd = 2;
static int log_color;
static struct timespec log_start;
static unsigned long log_dropped;
static int log_stalled;
static long long log_wait_until;

// the rest of a line the console took in part, written before any later line
static char log_pend[LOG_CON];
static size_t pend_at, pend_len;

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
	pend_at = pend_len = 0;
}

void log_adopt_fd(int fd)
{
	log_fd = fd;
	log_color = isatty(fd);
	log_set_nonblock();
	pend_at = pend_len = 0;
}

// the bytes the console accepts, waiting only as the batch allows. -1 on a hard error
static ssize_t log_push(const char *buf, size_t len, int wait)
{
	long long deadline = 0, now;
	size_t done = 0;

	while (done < len) {
		struct pollfd p = { .fd = log_fd, .events = POLLOUT };
		ssize_t w = write(log_fd, buf + done, len - done);

		if (w > 0) {
			done += (size_t)w;
			log_stalled = 0;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && errno != EAGAIN)
			return -1;
		if (!wait)
			break;
		now = log_now_ms();
		if (log_stalled || now >= log_wait_until)
			break;
		if (!deadline) {
			deadline = now + LOG_WRITE_MS;
			if (deadline > log_wait_until)
				deadline = log_wait_until;
		} else if (now >= deadline) {
			log_stalled = 1;
			break;
		}
		poll(&p, 1, 10);
	}
	return (ssize_t)done;
}

// a line reaches the console whole or not at all, the caller counts a line that does not
static int log_write(const char *buf, size_t len)
{
	ssize_t n;

	if (pend_len) {
		n = log_push(log_pend + pend_at, pend_len - pend_at, 1);
		if (n < 0 || pend_at + (size_t)n < pend_len) {
			if (n < 0)
				pend_at = pend_len = 0;
			else
				pend_at += (size_t)n;
			return 0;
		}
		pend_at = pend_len = 0;
	}
	n = log_push(buf, len, 1);
	if (n == (ssize_t)len)
		return 1;
	if (n <= 0 || len - (size_t)n > sizeof(log_pend))
		return 0;
	pend_len = len - (size_t)n;
	memcpy(log_pend, buf + n, pend_len);
	return 1;
}

int log_pending_fd(void)
{
	return pend_len ? log_fd : -1;
}

// called when the console is writable. a wakeup that writes nothing gives up the line
void log_flush(void)
{
	ssize_t n = log_push(log_pend + pend_at, pend_len - pend_at, 0);

	if (n > 0)
		pend_at += (size_t)n;
	if (n <= 0 || pend_at == pend_len) {
		if (n <= 0)
			log_dropped++;
		pend_at = pend_len = 0;
	}
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
	char buf[LOG_LINE], con[LOG_CON];
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

	// reporting before the console has caught up would take the place of real lines
	if (log_dropped && !pend_len) {
		unsigned long lost = log_dropped;

		if (log_report_dropped(lost))
			log_dropped -= lost;
	}
	if (!log_color || !p) {
		if (!log_write(buf, (size_t)n))
			log_dropped++;
		return;
	}
	c = prefix(con, sizeof(con), tag_color[level], ms);
	memcpy(con + c, buf + p, (size_t)(n - p));
	if (!log_write(con, (size_t)(c + n - p)))
		log_dropped++;
}

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
	struct utsname u;
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

	uname(&u);

	// the ring gets it without the colour
	n = fitted(snprintf(buf, sizeof(buf), "Welcome to %s! (%s)\n\n", os, u.release), 0, sizeof(buf));
	ring_put(buf, (size_t)n);
#ifndef NINIT_QUIET
	if (log_color)
		n = fitted(snprintf(buf, sizeof(buf), "Welcome to \033[1m%s\033[0m! (%s)\n\n",
				    os, u.release), 0, sizeof(buf));
	if (!log_write(buf, (size_t)n))
		log_dropped++;
#endif
}
