#pragma once

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned tap_n;

__attribute__((format(printf, 2, 3)))
static inline int tap_ok(int cond, const char *fmt, ...)
{
	va_list ap;

	tap_n++;
	printf("%sok %u - ", cond ? "" : "not ", tap_n);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	return cond;
}

__attribute__((format(printf, 1, 2)))
static inline void tap_diag(const char *fmt, ...)
{
	va_list ap;

	fputs("# ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

// a diagnostic line holds no newline, so multi-line strings are shown escaped
static inline void tap_diag_str(const char *what, const char *s, size_t n)
{
	fputs("#   ", stdout);
	fputs(what, stdout);
	if (!s) {
		fputs("(null)\n", stdout);
		return;
	}
	putchar('"');
	for (size_t k = 0; k < n; k++) {
		unsigned char c = (unsigned char)s[k];

		if (c == '\n')
			fputs("\\n", stdout);
		else if (c == '\t')
			fputs("\\t", stdout);
		else if (c == '"' || c == '\\')
			printf("\\%c", c);
		else if (c < ' ' || c == 0x7f)
			printf("\\x%02x", c);
		else
			putchar(c);
	}
	fputs("\"\n", stdout);
}

static inline int tap_is_int(long long got, long long want, const char *what)
{
	if (tap_ok(got == want, "%s", what))
		return 1;
	tap_diag("  got %lld, want %lld", got, want);
	return 0;
}

static inline int tap_is_str(const char *got, const char *want, const char *what)
{
	int same = got && want ? !strcmp(got, want) : got == want;

	if (tap_ok(same, "%s", what))
		return 1;
	tap_diag_str("got:  ", got, got ? strlen(got) : 0);
	tap_diag_str("want: ", want, want ? strlen(want) : 0);
	return 0;
}

static inline int tap_is_mem(const void *got, size_t gn, const void *want, size_t wn,
			     const char *what)
{
	if (tap_ok(gn == wn && !memcmp(got, want, gn), "%s", what))
		return 1;
	tap_diag_str("got:  ", (const char *)got, gn);
	tap_diag_str("want: ", (const char *)want, wn);
	return 0;
}

static inline int tap_has_str(const char *hay, const char *needle, const char *what)
{
	if (tap_ok(hay && strstr(hay, needle), "%s", what))
		return 1;
	tap_diag_str("in:   ", hay, hay ? strlen(hay) : 0);
	tap_diag_str("want: ", needle, strlen(needle));
	return 0;
}

static inline int tap_done(void)
{
	printf("1..%u\n", tap_n);
	fflush(stdout);
	return 0;
}

#define ok(c, ...)		tap_ok(!!(c), __VA_ARGS__)
#define is_int(g, w, s)		tap_is_int((long long)(g), (long long)(w), s)
#define is_str(g, w, s)		tap_is_str(g, w, s)
#define has_str(h, n, s)	tap_has_str(h, n, s)

// deterministic so a failure reproduces, xorshift64*
static uint64_t tap_rng_state = 0x9e3779b97f4a7c15ull;

static inline uint64_t tap_rand(void)
{
	uint64_t x = tap_rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	tap_rng_state = x;
	return x * 0x2545f4914f6cdd1dull;
}

static inline uint32_t tap_below(uint32_t n)
{
	return n ? (uint32_t)(tap_rand() % n) : 0;
}
