#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "tap.h"

// the log timestamps are checked against a clock the test sets
static int fake_clock;
static long long fake_ms;

static int shim_clock_gettime(clockid_t id, struct timespec *ts)
{
	if (!fake_clock)
		return clock_gettime(id, ts);
	ts->tv_sec = fake_ms / 1000;
	ts->tv_nsec = (fake_ms % 1000) * 1000000;
	return 0;
}

static const char *osrel_etc, *osrel_lib, *console_path;

static FILE *shim_fopen(const char *path, const char *mode)
{
	if (!strcmp(path, "/etc/os-release"))
		path = osrel_etc;
	else if (!strcmp(path, "/usr/lib/os-release"))
		path = osrel_lib;
	if (!path) {
		errno = ENOENT;
		return NULL;
	}
	return fopen(path, mode);
}

static int shim_open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/dev/console")) {
		if (!console_path) {
			errno = ENOENT;
			return -1;
		}
		path = console_path;
	}
	return open(path, flags);
}

// bytes the console still accepts, -1 for no limit, makes partial writes exact
static long con_budget = -1;
static int con_w = -1;

static ssize_t shim_write(int fd, const void *buf, size_t n)
{
	if (fd == con_w && con_budget >= 0) {
		if (!con_budget) {
			errno = EAGAIN;
			return -1;
		}
		if (n > (size_t)con_budget)
			n = (size_t)con_budget;
		con_budget -= (long)n;
	}
	return write(fd, buf, n);
}

static int shim_poll(struct pollfd *p, nfds_t n, int ms)
{
	if (n == 1 && p->fd == con_w && con_budget == 0) {
		usleep((useconds_t)ms * 1000);
		return 0;
	}
	return poll(p, n, ms);
}

#define clock_gettime shim_clock_gettime
#define fopen shim_fopen
#define open shim_open
#define write shim_write
#define poll shim_poll
#include "../../src/logging.c"
#undef clock_gettime
#undef fopen
#undef open
#undef write
#undef poll

static int con_r = -1;

static void console_pipe(int size)
{
	int p[2];

	if (con_r >= 0)
		close(con_r);
	if (con_w >= 0)
		close(con_w);
	if (pipe2(p, O_CLOEXEC) < 0)
		abort();
	if (size)
		fcntl(p[1], F_SETPIPE_SZ, size);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	con_r = p[0];
	con_w = p[1];
	log_adopt_fd(con_w);
}

static size_t console_read(char *buf, size_t cap)
{
	size_t at = 0;

	for (;;) {
		ssize_t k = read(con_r, buf + at, cap - 1 - at);

		if (k <= 0)
			break;
		at += (size_t)k;
	}
	buf[at] = '\0';
	return at;
}

// fills the console pipe and returns how many bytes it took
static size_t console_fill(size_t leave)
{
	static char junk[65536];
	size_t total = 0;
	int cap = fcntl(con_w, F_GETPIPE_SZ);

	memset(junk, 'j', sizeof(junk));
	if (cap > 0 && leave) {
		size_t want = (size_t)cap - leave;
		ssize_t k = write(con_w, junk, want);

		return k > 0 ? (size_t)k : 0;
	}
	for (;;) {
		ssize_t k = write(con_w, junk, sizeof(junk));

		if (k <= 0)
			break;
		total += (size_t)k;
	}
	return total;
}

static void console_drain(void)
{
	char buf[4096];

	while (read(con_r, buf, sizeof(buf)) > 0)
		;
}

static int matches(const char *s, const char *re)
{
	regex_t r;
	int m;

	if (regcomp(&r, re, REG_EXTENDED | REG_NOSUB))
		abort();
	m = !regexec(&r, s, 0, NULL, 0);
	regfree(&r);
	return m;
}

// the last line in the ring
static const char *ring_last(void)
{
	static char buf[LOG_LINE + 1];
	uint64_t at = log_first();
	size_t n = 0;

	while (at < log_end())
		n = log_line(&at, buf);
	buf[n] = '\0';
	return buf;
}

static void test_format(void)
{
	char con[8192];
	const char *line;

	console_pipe(0);
	fake_clock = 1;
	fake_ms = 1000;
	log_init();
	log_adopt_fd(con_w);

	fake_ms = 1000 + 1234;
	ninit_log(LOG_NOTE, "hello %d", 5);
	line = ring_last();
	is_str(line, "[001.234] NOTE > hello 5\n", "a line is [SSS.mmm] TAG > message");
	console_read(con, sizeof(con));
	is_str(con, "[001.234] NOTE > hello 5\n", "the console gets the same line uncoloured");

	fake_ms = 1000;
	ninit_log(LOG_DONE, "a");
	is_str(ring_last(), "[000.000] DONE > a\n", "DONE tag and a zero time");
	ninit_log(LOG_WARN, "b");
	is_str(ring_last(), "[000.000] WARN > b\n", "WARN tag");
	ninit_log(LOG_FAIL, "c");
	is_str(ring_last(), "[000.000] FAIL > c\n", "FAIL tag");
	ninit_log(LOG_WAIT, "d");
	is_str(ring_last(), "[000.000] WAIT > d\n", "WAIT tag");
	ninit_log(LOG_INFO, "e");
	is_str(ring_last(), "e\n", "INFO has no prefix");
	ninit_log(9, "f");
	is_str(ring_last(), "f\n", "an unknown level is logged as INFO");

	fake_ms = 1000 + 1234567;
	ninit_log(LOG_NOTE, "late");
	is_str(ring_last(), "[1234.567] NOTE > late\n", "seconds widen past 999");
	fake_ms = 1000 + 59;
	ninit_log(LOG_NOTE, "pad");
	is_str(ring_last(), "[000.059] NOTE > pad\n", "milliseconds are zero padded");
	fake_ms = 0;
	ninit_log(LOG_NOTE, "past");
	is_str(ring_last(), "[000.000] NOTE > past\n", "a clock behind the start reads as zero");
	fake_ms = 1000;

	console_read(con, sizeof(con));
	ninit_log(LOG_NOTE | LOG_NOCON, "ring only");
	is_str(ring_last(), "[000.000] NOTE > ring only\n", "LOG_NOCON reaches the ring");
	is_int(console_read(con, sizeof(con)), 0, "LOG_NOCON stays off the console");

	{
		char big[5000];
		size_t n;

		memset(big, 'x', sizeof(big) - 1);
		big[sizeof(big) - 1] = '\0';
		ninit_log(LOG_NOTE, "%s", big);
		line = ring_last();
		n = strlen(line);
		is_int(n, LOG_LINE - 1, "a long line is cut to fit LOG_LINE with its newline");
		ok(n && line[n - 1] == '\n', "a cut line still ends in a newline");
		console_read(con, sizeof(con));
		is_int(strlen(con), LOG_LINE - 1, "the console gets the same cut line");
	}

	{
		uint64_t at = log_end();
		char b[LOG_LINE + 1];
		size_t n;

		log_raw(LOG_FAIL, "x\n\ny\n", 5);
		n = log_line(&at, b);
		b[n] = '\0';
		is_str(b, "[000.000] FAIL > x\n", "log_raw splits on newlines");
		n = log_line(&at, b);
		b[n] = '\0';
		is_str(b, "[000.000] FAIL > y\n", "log_raw skips empty lines");
		is_int(log_line(&at, b), 0, "and nothing follows");
	}
	console_read(con, sizeof(con));
	log_raw(LOG_NOTE, "tail", 4);
	is_str(ring_last(), "[000.000] NOTE > tail\n", "log_raw takes text without a final newline");
	log_raw(LOG_NOTE, "", 0);
	is_str(ring_last(), "[000.000] NOTE > tail\n", "log_raw of nothing logs nothing");
	fake_clock = 0;
	console_read(con, sizeof(con));
}

// the ring from at onwards
static const char *ring_since(uint64_t at)
{
	static char buf[32768];
	size_t n = 0;

	while (at < log_end() && n + LOG_LINE + 1 < sizeof(buf))
		n += log_line(&at, buf + n);
	buf[n] = '\0';
	return buf;
}

// a line no other equals ends any repetition, returns where the ring continues
static uint64_t rep_reset(void)
{
	static unsigned n;

	ninit_log(LOG_NOTE | LOG_NOCON, "reset %u", ++n);
	return log_end();
}

static void test_repeat(void)
{
	static const char *const block[] = { "iwd: a", "iwd: b", "iwd: c", "iwd: d" };
	char con[8192], want[2048];
	uint64_t at;
	size_t w;

	console_pipe(0);
	fake_clock = 1;
	fake_ms = 0;
	log_init();
	log_adopt_fd(con_w);

	at = rep_reset();
	for (int k = 0; k < 5; k++) {
		fake_ms = k;
		ninit_log(LOG_NOTE, "same");
	}
	is_str(ring_since(at), "[000.000] NOTE > same\n", "a line logged again is held back");
	ok(log_due() >= 0, "and a count is pending");
	fake_ms = 10;
	ninit_log(LOG_NOTE, "other");
	is_str(ring_since(at), "[000.000] NOTE > same\n"
			       "[000.004] NOTE > last message repeated 4 times\n"
			       "[000.010] NOTE > other\n",
	       "the count precedes the next line, at the time of the last repeat");
	console_read(con, sizeof(con));
	is_str(con, ring_since(at), "the console gets the same lines");
	is_int(log_due(), -1, "nothing is pending after the count");

	at = rep_reset();
	ninit_log(LOG_NOTE, "twice");
	ninit_log(LOG_NOTE, "twice");
	ninit_log(LOG_NOTE, "after");
	is_str(ring_since(at), "[000.010] NOTE > twice\n[000.010] NOTE > twice\n[000.010] NOTE > after\n",
	       "a line repeated once is logged as it was");

	at = rep_reset();
	ninit_log(LOG_NOTE, "lv");
	ninit_log(LOG_WARN, "lv");
	is_str(ring_since(at), "[000.010] NOTE > lv\n[000.010] WARN > lv\n",
	       "the same text at another level is not a repeat");

	at = rep_reset();
	ninit_log(LOG_NOTE, "x");
	ninit_log(LOG_NOTE, "y");
	ninit_log(LOG_NOTE, "x");
	is_str(ring_since(at), "[000.010] NOTE > x\n[000.010] NOTE > y\n[000.010] NOTE > x\n",
	       "a line that recurs is logged at once");
	is_int(log_due(), -1, "and nothing is held");

	// the iwd case, a block of lines repeating every few seconds
	at = rep_reset();
	for (int r = 0; r < 10; r++) {
		fake_ms = 1000 + r * 100;
		for (int k = 0; k < 4; k++)
			ninit_log(LOG_NOTE, "%s", block[k]);
	}
	fake_ms = 3000;
	ninit_log(LOG_NOTE, "end");
	w = 0;
	for (int r = 0; r < 2; r++)
		for (int k = 0; k < 4; k++)
			w += (size_t)snprintf(want + w, sizeof(want) - w, "[001.%03d] NOTE > %s\n",
					      r * 100, block[k]);
	snprintf(want + w, sizeof(want) - w, "[001.900] NOTE > last 4 messages repeated 8 times\n"
					     "[003.000] NOTE > end\n");
	is_str(ring_since(at), want, "a block logged twice in a row is counted from then on");

	// a repetition cut short is logged as it arrived
	at = rep_reset();
	for (int r = 0; r < 3; r++)
		for (int k = 0; k < 4; k++) {
			fake_ms = 5000 + r * 10 + k;
			ninit_log(LOG_NOTE, "%s", block[k]);
		}
	fake_ms = 5100;
	ninit_log(LOG_NOTE, "%s", block[0]);
	fake_ms = 5101;
	ninit_log(LOG_NOTE, "%s", block[1]);
	fake_ms = 5200;
	ninit_log(LOG_NOTE, "z");
	w = 0;
	for (int r = 0; r < 3; r++)
		for (int k = 0; k < 4; k++)
			w += (size_t)snprintf(want + w, sizeof(want) - w, "[005.%03d] NOTE > %s\n",
					      r * 10 + k, block[k]);
	snprintf(want + w, sizeof(want) - w, "[005.100] NOTE > iwd: a\n[005.101] NOTE > iwd: b\n"
					     "[005.200] NOTE > z\n");
	is_str(ring_since(at), want, "a single repetition and a partial one are logged with their times");

	// a held line is logged after LOG_HOLD_MS when nothing completes the block
	at = rep_reset();
	fake_ms = 6000;
	for (int r = 0; r < 2; r++)
		for (int k = 0; k < 4; k++)
			ninit_log(LOG_NOTE, "%s", block[k]);
	fake_ms = 6050;
	ninit_log(LOG_NOTE, "%s", block[0]);
	has_str(ring_last(), "iwd: d", "the start of a third repetition is held");
	is_int(log_due(), LOG_HOLD_MS, "for LOG_HOLD_MS");
	fake_ms = 6050 + LOG_HOLD_MS - 1;
	log_tick();
	has_str(ring_last(), "iwd: d", "and not sooner");
	fake_ms = 6050 + LOG_HOLD_MS;
	log_tick();
	is_str(ring_last(), "[006.050] NOTE > iwd: a\n", "then logged with the time it arrived");
	is_int(log_due(), -1, "and nothing is pending");

	// the count is logged after 30 s, 120 s, then every 600 s
	at = rep_reset();
	fake_ms = 10000;
	for (int k = 0; k < 4; k++) {
		fake_ms = 10000 + k;
		ninit_log(LOG_WARN, "flood");
	}
	is_int(log_due(), 29998, "a count is due 30 s after the first repeat");
	fake_ms = 40001;
	log_tick();
	is_str(ring_since(at), "[010.000] WARN > flood\n[010.003] WARN > last message repeated 3 times\n",
	       "and is logged then, at the level of the line");
	fake_ms = 50000;
	ninit_log(LOG_WARN, "flood");
	ninit_log(LOG_WARN, "flood");
	is_int(log_due(), 120000, "the next count is due after 120 s");
	at = log_end();
	fake_ms = 170000;
	log_tick();
	is_str(ring_since(at), "[050.000] WARN > last message repeated 2 times\n",
	       "the repeat goes on being counted after a count");
	fake_ms = 170001;
	ninit_log(LOG_WARN, "flood");
	is_int(log_due(), 600000, "and the interval stops at 600 s");

	// the count takes the most severe tag of its block
	at = rep_reset();
	fake_ms = 0;
	for (int r = 0; r < 4; r++) {
		ninit_log(LOG_NOTE, "n");
		ninit_log(LOG_WARN, "w");
	}
	ninit_log(LOG_NOTE, "done");
	is_str(ring_since(at), "[000.000] NOTE > n\n[000.000] WARN > w\n[000.000] NOTE > n\n[000.000] WARN > w\n"
			       "[000.000] WARN > last 2 messages repeated 2 times\n[000.000] NOTE > done\n",
	       "a block with a WARN line is counted as WARN");

	console_read(con, sizeof(con));
	at = rep_reset();
	for (int k = 0; k < 3; k++)
		ninit_log(LOG_NOTE | LOG_NOCON, "quiet");
	rep_reset();
	has_str(ring_since(at), "NOTE > last message repeated 2 times\n", "a ring-only line is counted in the ring");
	is_int(console_read(con, sizeof(con)), 0, "and its count stays off the console");

	rep_reset();
	fake_clock = 0;
}

static unsigned feeds;

static void count_feed(void)
{
	feeds++;
}

// each line carries its number and a length derived from it, so corruption shows
static void make_line(char *buf, size_t cap, unsigned seq)
{
	unsigned pad = (seq * 37u) % 300u;

	snprintf(buf, cap, "seq %u pad %u ", seq, pad);
	for (size_t k = strlen(buf); pad-- && k + 1 < cap; k++) {
		buf[k] = (char)('a' + (seq + k) % 26);
		buf[k + 1] = '\0';
	}
}

static int check_line(const char *line, unsigned *seq)
{
	char want[512];
	const char *p = strstr(line, "seq ");
	size_t n;

	if (!p || sscanf(p, "seq %u", seq) != 1)
		return 0;
	make_line(want, sizeof(want), *seq);
	n = strlen(want);
	return !strncmp(p, want, n) && p[n] == '\n' && !p[n + 1];
}

static void test_ring(void)
{
	char line[512], buf[LOG_LINE + 1];
	uint64_t start, at;
	unsigned seq, first = 0, last = 0, count = 0, bad = 0, expect;

	console_pipe(0);
	log_feed(count_feed);
	feeds = 0;
	start = log_end();
	for (unsigned k = 0; k < 2000; k++) {
		make_line(line, sizeof(line), k);
		ninit_log(LOG_NOTE | LOG_NOCON, "%s", line);
	}
	is_int(feeds, 2000, "the feed hook runs after every append");
	log_feed(NULL);

	ok(log_end() - start > LOG_RING, "more than a ring's worth was logged");
	ok(log_end() - log_first() <= LOG_RING, "the ring holds at most LOG_RING bytes");

	at = log_first();
	expect = UINT32_MAX;
	while (at < log_end()) {
		size_t n = log_line(&at, buf);

		buf[n] = '\0';
		if (!check_line(buf, &seq)) {
			if (bad++ < 3)
				tap_diag_str("corrupt line: ", buf, n);
			continue;
		}
		if (!count)
			first = seq;
		else if (seq != expect && bad++ < 3)
			tap_diag("line %u follows %u", seq, expect - 1);
		expect = seq + 1;
		last = seq;
		count++;
	}
	ok(!bad, "every line in the ring is whole and in order across the wrap");
	is_int(last, 1999, "the newest line is kept");
	ok(first > 0, "the oldest lines were dropped (first kept is %u)", first);

	// a reader the ring has overtaken is told how much it lost
	at = start;
	{
		size_t n = log_line(&at, buf);
		char want[64];

		buf[n] = '\0';
		snprintf(want, sizeof(want), "log: skipped %llu bytes\n",
			 (unsigned long long)(log_first() - start));
		ok(matches(buf, "^\\[[0-9]{3,}\\.[0-9]{3}\\] WARN > log: skipped [0-9]+ bytes\n$"),
		   "an overtaken reader gets a WARN line");
		has_str(buf, want, "the skip counts the bytes lost");
		is_int(at, log_first(), "and resumes at the oldest line");
	}
	at = log_end();
	is_int(log_line(&at, buf), 0, "a reader at the end gets nothing");

	// lines of every length wrap at every offset
	bad = 0;
	for (unsigned k = 0; k < 3000; k++) {
		uint64_t mark = log_end();
		size_t n;

		make_line(line, sizeof(line), 7000 + k * 13);
		ninit_log(LOG_INFO | LOG_NOCON, "%s", line);
		n = log_line(&mark, buf);
		buf[n] = '\0';
		if (!check_line(buf, &seq) && bad++ < 3)
			tap_diag_str("reread: ", buf, n);
	}
	ok(!bad, "a line reads back intact wherever it lands in the ring");
}

static void test_console_stall(void)
{
	char con[65536 * 4];
	long long t0, t1;
	size_t n;

	console_pipe(4096);
	log_batch_begin();
	console_fill(0);

	t0 = log_now_ms();
	ninit_log(LOG_NOTE, "stalled one");
	t1 = log_now_ms();
	ok(t1 - t0 >= LOG_WRITE_MS - 5 && t1 - t0 < LOG_WRITE_MS + 200,
	   "a full console is waited on for about %d ms (%lld)", LOG_WRITE_MS, t1 - t0);
	is_int(log_dropped, 1, "the line it did not take is dropped and counted");

	t0 = log_now_ms();
	for (int k = 0; k < 5; k++)
		ninit_log(LOG_NOTE, "stalled more %d", k);
	t1 = log_now_ms();
	ok(t1 - t0 < 50, "once stalled no further line waits (%lld ms)", t1 - t0);
	is_int(log_dropped, 6, "each dropped line is counted");
	has_str(ring_last(), "stalled more 4", "the ring kept the dropped lines");

	console_drain();
	log_batch_begin();
	ninit_log(LOG_NOTE, "resumed");
	n = console_read(con, sizeof(con));
	ok(matches(con, "^\\[[0-9.]+\\] WARN > console: dropped [0-9]+ messages\n\\[[0-9.]+\\] NOTE > resumed\n$"),
	   "the drop count is reported before the next line");
	if (!matches(con, "dropped 6 messages"))
		tap_diag_str("console: ", con, n);
	is_int(log_dropped, 0, "the count is reset once reported");

	// a single drop reads as one message
	console_fill(0);
	log_batch_begin();
	log_stalled = 1;
	ninit_log(LOG_NOTE, "x");
	console_drain();
	log_batch_begin();
	ninit_log(LOG_NOTE, "y");
	console_read(con, sizeof(con));
	has_str(con, "console: dropped 1 message\n", "one dropped line is singular");

	// the batch budget caps the total wait across several lines
	console_fill(0);
	log_batch_begin();
	log_wait_until = log_now_ms() + 30;
	t0 = log_now_ms();
	ninit_log(LOG_NOTE, "short budget");
	t1 = log_now_ms();
	ok(t1 - t0 < LOG_WRITE_MS, "a nearly spent batch waits less than %d ms (%lld)",
	   LOG_WRITE_MS, t1 - t0);
	console_drain();
	log_batch_begin();
	ninit_log(LOG_NOTE, "flush");
	console_read(con, sizeof(con));
}

// the console receives whole lines only
static int whole_lines(const char *s, size_t n, const char **bad)
{
	const char *p = s, *end = s + n;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));

		if (!nl || !matches(p, "^\\[[0-9]{3,}\\.[0-9]{3}\\] [A-Z]{4} > ")) {
			*bad = p;
			return 0;
		}
		p = nl + 1;
	}
	return 1;
}

static void test_console_partial(void)
{
	char con[65536], line[200];
	const char *bad;
	size_t n;

	console_pipe(0);
	memset(line, 'p', 80);
	line[80] = '\0';

	con_budget = 10;
	log_batch_begin();
	ninit_log(LOG_NOTE, "%s", line);
	ok(log_pending_fd() == con_w, "a line the console accepted in part is pending");
	is_int(log_dropped, 0, "a partly written line is not counted as dropped");

	ninit_log(LOG_NOTE, "behind the pending one");
	is_int(log_dropped, 1, "a line behind a pending one is dropped");
	ok(log_pending_fd() == con_w, "the pending remainder is kept");

	con_budget = -1;
	log_flush();
	ok(log_pending_fd() < 0, "log_flush completes the pending line");
	log_batch_begin();
	ninit_log(LOG_NOTE, "next");
	n = console_read(con, sizeof(con));
	bad = NULL;
	ok(whole_lines(con, n, &bad), "the console received only whole lines");
	if (bad)
		tap_diag_str("at: ", bad, strlen(bad));
	ok(matches(con, "^\\[[0-9.]+\\] NOTE > p{80}\n"), "the partial line arrived whole");
	has_str(con, "console: dropped 1 message\n", "the dropped line is reported");
	has_str(con, "NOTE > next\n", "logging continues");
	ok(!strstr(con, "behind the pending"), "the dropped line is absent from the console");

	// no wait left in the batch, the remainder is written before any later line
	con_budget = 5;
	log_batch_begin();
	log_wait_until = log_now_ms();
	ninit_log(LOG_NOTE, "%s", line);
	ok(log_pending_fd() == con_w, "a line accepted in part with no wait left is pending");
	con_budget = -1;
	log_batch_begin();
	ninit_log(LOG_NOTE, "after pending");
	n = console_read(con, sizeof(con));
	ok(log_pending_fd() < 0, "a later line writes the pending remainder first");
	ok(matches(con, "^\\[[0-9.]+\\] NOTE > p{80}\n\\[[0-9.]+\\] NOTE > after pending\n$"),
	   "the remainder precedes the later line");
	is_int(log_dropped, 0, "nothing was dropped");

	// a writable wakeup that writes nothing abandons the rest of the line
	con_budget = 3;
	log_batch_begin();
	log_wait_until = log_now_ms();
	ninit_log(LOG_NOTE, "%s", line);
	con_budget = 0;
	log_flush();
	ok(log_pending_fd() < 0, "log_flush with no progress clears the pending line");
	is_int(log_dropped, 1, "the abandoned line is counted");
	con_budget = -1;
	log_dropped = 0;
	console_read(con, sizeof(con));

	// a line the console accepts none of is dropped, not left pending
	{
		char big[LOG_LINE];

		memset(big, 'b', sizeof(big) - 1);
		big[sizeof(big) - 1] = '\0';
		con_budget = 0;
		log_batch_begin();
		log_wait_until = log_now_ms();
		ninit_log(LOG_NOTE, "%s", big);
		ok(log_pending_fd() < 0, "a line the console took nothing of is not pending");
		is_int(log_dropped, 1, "and is dropped");
		con_budget = -1;
		log_dropped = 0;
	}

	// a write error drops the console line but not the ring line
	{
		int p[2];

		if (pipe2(p, O_CLOEXEC) < 0)
			abort();
		close(p[0]);
		log_adopt_fd(p[1]);
		signal(SIGPIPE, SIG_IGN);
		ninit_log(LOG_NOTE, "into a closed pipe");
		has_str(ring_last(), "into a closed pipe", "a console error keeps the ring line");
		is_int(log_dropped, 1, "and counts the console line as dropped");
		close(p[1]);
		log_adopt_fd(con_w);
		log_dropped = 0;
	}
}

static void test_tty(void)
{
	int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC), s;
	char buf[1024];
	ssize_t n;

	if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0) {
		ok(1, "# SKIP no pty available");
		return;
	}
	s = open(ptsname(m), O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (s < 0) {
		ok(1, "# SKIP no pty available");
		return;
	}
	{
		struct termios t;

		tcgetattr(s, &t);
		cfmakeraw(&t);
		tcsetattr(s, TCSANOW, &t);
	}
	log_adopt_fd(s);
	ok(log_color, "a terminal console is coloured");
	fake_clock = 1;
	fake_ms = 0;
	log_init();
	log_adopt_fd(s);
	ninit_log(LOG_NOTE, "colour");
	n = read(m, buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = '\0';
	is_str(buf, "[000.000] \033[36mNOTE\033[0m > colour\n", "NOTE is cyan on a terminal");
	is_str(ring_last(), "[000.000] NOTE > colour\n", "the ring never holds colour codes");

	{
		static const char *const want[] = {
			"[000.000] \033[32mDONE\033[0m > t\n",
			"t\n",
			"[000.000] \033[33mWARN\033[0m > t\n",
			"[000.000] \033[31mFAIL\033[0m > t\n",
			"[000.000] \033[1;34mWAIT\033[0m > t\n",
		};

		for (int lv = 0; lv < 5; lv++) {
			char what[32];

			ninit_log(lv, "t");
			n = read(m, buf, sizeof(buf) - 1);
			buf[n > 0 ? n : 0] = '\0';
			snprintf(what, sizeof(what), "level %d colour", lv);
			is_str(buf, want[lv], what);
		}
	}
	fake_clock = 0;
	close(s);
	close(m);
	log_adopt_fd(con_w);
}

static char tmpdir[] = "/tmp/ninit-t-logging.XXXXXX";

static const char *put(const char *name, const char *text)
{
	static char paths[8][256];
	static int k;
	char *p = paths[k++ & 7];
	FILE *f;

	snprintf(p, 256, "%s/%s", tmpdir, name);
	f = fopen(p, "w");
	if (!f)
		abort();
	fputs(text, f);
	fclose(f);
	return p;
}

static void welcome_case(const char *what, const char *etc, const char *lib, const char *want_os)
{
	struct utsname u;
	char want[512], con[2048], got[LOG_LINE + 1];
	uint64_t at = log_end();
	size_t n;

	osrel_etc = etc;
	osrel_lib = lib;
	uname(&u);
	snprintf(want, sizeof(want), "Welcome to %s! (%s)\n", want_os, u.release);
	console_read(con, sizeof(con));
	print_welcome();
	n = log_line(&at, got);
	got[n] = '\0';
	is_str(got, want, what);
	n = log_line(&at, got);
	got[n] = '\0';
	is_str(got, "\n", "the welcome is followed by a blank line");
	n = console_read(con, sizeof(con));
#ifdef NINIT_QUIET
	is_int(n, 0, "a quiet build keeps the welcome off the console");
#else
	strcat(want, "\n");
	is_str(con, want, "the console gets the welcome too");
#endif
}

static void test_welcome(void)
{
	if (!mkdtemp(tmpdir))
		abort();
	console_pipe(0);
	welcome_case("PRETTY_NAME in double quotes",
		     put("a", "NAME=x\nPRETTY_NAME=\"Gentoo Linux\"\nID=gentoo\n"), NULL, "Gentoo Linux");
	welcome_case("PRETTY_NAME in single quotes", put("b", "PRETTY_NAME='Void'\n"), NULL, "Void");
	welcome_case("PRETTY_NAME unquoted", put("c", "PRETTY_NAME=Alpine\n"), NULL, "Alpine");
	welcome_case("PRETTY_NAME without a newline", put("d", "PRETTY_NAME=\"Last\""), NULL, "Last");
	welcome_case("no PRETTY_NAME", put("e", "NAME=x\n"), NULL, "Linux");
	welcome_case("no os-release", NULL, NULL, "Linux");
	welcome_case("/usr/lib/os-release when /etc has none", NULL,
		     put("f", "PRETTY_NAME=\"From lib\"\n"), "From lib");
	welcome_case("/etc/os-release wins", put("g", "PRETTY_NAME=etc\n"),
		     put("h", "PRETTY_NAME=lib\n"), "etc");
	welcome_case("the first PRETTY_NAME wins", put("i", "PRETTY_NAME=one\nPRETTY_NAME=two\n"), NULL,
		     "one");
	osrel_etc = osrel_lib = NULL;
}

static void test_reopen(void)
{
	char con[256];
	const char *path = put("console", "");
	int fd;

	console_path = NULL;
	console_pipe(0);
	log_reopen_console();
	ninit_log(LOG_WARN, "still the pipe");
	console_read(con, sizeof(con));
	has_str(con, "still the pipe", "a console that cannot be opened keeps the old fd");

	console_path = path;
	log_reopen_console();
	ninit_log(LOG_WARN, "to the file");
	fd = open(path, O_RDONLY | O_CLOEXEC);
	{
		ssize_t n = read(fd, con, sizeof(con) - 1);

		con[n > 0 ? n : 0] = '\0';
	}
	close(fd);
	has_str(con, "WARN > to the file", "log_reopen_console moves the log to /dev/console");
	ok(fcntl(log_fd, F_GETFL) & O_NONBLOCK, "the console is non-blocking");
	// reopening closed the pipe end it replaced
	con_w = -1;
	close(log_fd);
	unlink(path);
	console_path = NULL;
	console_pipe(0);
}

#ifdef NINIT_QUIET
static void test_quiet(void)
{
	char con[1024];

	console_pipe(0);
	log_note("quiet note");
	log_info("quiet info");
	log_done("quiet done");
	log_wait("quiet wait");
	log_warn("loud warn");
	log_err("loud fail");
	console_read(con, sizeof(con));
	ok(!strstr(con, "quiet"), "a quiet build prints no NOTE, INFO, DONE or WAIT");
	has_str(con, "WARN > loud warn", "but prints WARN");
	has_str(con, "FAIL > loud fail", "and FAIL");
	has_str(ring_last(), "loud fail", "the ring gets everything");
}
#endif

int main(void)
{
	log_init();
	test_format();
	test_repeat();
	test_ring();
	test_console_stall();
	test_console_partial();
	test_tty();
	test_welcome();
	test_reopen();
#ifdef NINIT_QUIET
	test_quiet();
#endif
	rmdir(tmpdir);
	return tap_done();
}
