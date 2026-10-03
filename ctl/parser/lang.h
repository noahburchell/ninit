#pragma once

#include <stddef.h>

struct src;

struct lang {
	const char *name;
	// rewrites the script in place for storage, returns its length
	size_t (*store)(struct src *s, char *body, size_t len, size_t code_off);
	// fills argv to check the syntax of a script read from stdin, returns the program to run
	const char *(*check_argv)(const struct src *s, char **argv, size_t cap);
};

extern const struct lang lang_shell;

extern int g_sh_lexed;

const struct lang *lang_select(struct src *s, const char *fname, const char *body);
int sh_lexed(const char *argv0);
