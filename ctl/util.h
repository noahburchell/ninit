#pragma once

#include <stddef.h>
#include <stdint.h>

// the vector owns its array, never the strings, which live until ninitctl exits
struct strv {
	const char **v;
	uint32_t n, cap;
};

extern const char *g_dir;
// the --root directory or NULL, and the prefix that shows a path below it, empty without one
extern const char *g_root;
extern const char *g_root_pfx;

struct stat;

void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void usage_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void strv_push(struct strv *s, const char *v);
void set_root(const char *dir);
int sys_open(const char *path, int flags);
int sys_stat(const char *path, struct stat *st);
int sys_exec_ok(const char *path, const struct stat *st);
char *sys_realpath(const char *dir);
char *slurp(const char *path, size_t *len);
char *xstrndup(const char *s, size_t n);
