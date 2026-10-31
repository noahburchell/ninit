#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// a view of bytes it does not own, without a terminating nul
typedef struct str {
	const char* p;
	size_t      n;
} str;

// STR_INIT is the form a static initializer takes
#define STR_INIT(lit) { "" lit, sizeof(lit) - 1 }
#define STR(lit)      ((str)STR_INIT(lit))

static inline str str_c(const char* s) {
	return (str){ s, strlen(s) };
}

static inline bool str_eq(str a, str b) {
	return a.n == b.n && !memcmp(a.p, b.p, a.n);
}

static inline bool str_prefix(str s, str pre) {
	return s.n >= pre.n && !memcmp(s.p, pre.p, pre.n);
}

// the text after the last slash
static inline str str_base(str s) {
	const char* b = memrchr(s.p, '/', s.n);

	return b ? (str){ b + 1, s.n - (size_t)(b + 1 - s.p) } : s;
}

// the text before the first C, S keeps the text after it
static inline str str_cut(str* s, char c) {
	const char* e = memchr(s->p, c, s->n);
	str t = { s->p, e ? (size_t)(e - s->p) : s->n };
	size_t skip = t.n + (e != NULL);

	s->p += skip;
	s->n -= skip;

	return t;
}

static inline const char* path_base(const char* path) {
	const char* b = strrchr(path, '/');

	return b ? b + 1 : path;
}

static inline const char* plural(uint64_t n) {
	return n == 1 ? "" : "s";
}
