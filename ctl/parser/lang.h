#pragma once

#include <stddef.h>

struct src;

struct lang {
	const char *name;
	// true for the basename of a #! interpreter this language runs
	int (*claims)(const char *base, size_t len);
	// rewrites the script in place for storage, returns its length
	size_t (*store)(struct src *s, char *body, size_t len, size_t code_off);
	// fills argv to check the syntax of a script read from stdin, returns the program to run
	const char *(*check_argv)(const struct src *s, char **argv, size_t cap);
	// fills s->exec_pre and s->exec_suf, the argv ninit puts around the script
	void (*exec_args)(struct src *s);
};

extern const struct lang lang_shell, lang_python, lang_lua, lang_perl;

extern int g_sh_lexed;

const struct lang *lang_select(struct src *s, const char *fname, const char *body);
int sh_lexed(const char *argv0);
size_t blank_header(struct src *s, char *body, size_t len, size_t code_off);
int claims_versioned(const char *base, size_t len, const char *name);
void check_exec_args(const struct src *s, const char *fname);
