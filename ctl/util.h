#pragma once

#include <stddef.h>
#include <stdint.h>

struct strv {
	char **v;
	uint32_t n, cap;
};

extern const char *g_dir;

void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void usage_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void strv_push(struct strv *s, char *v);
char *slurp(const char *path, size_t *len);
