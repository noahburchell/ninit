#pragma once

#include "../../help/str.h"

#include <stddef.h>

struct src;
struct strv;

// why a #! argument is refused
#define ARG_SCRIPT "replaces the script"
#define ARG_CODE   "adds code outside the script"
#define ARG_EXIT   "exits before the script runs"
#define ARG_LOOP   "loops the script over stdin, which is /dev/null"

struct lang {
	const char* name;
	// true for the basename of a #! interpreter this language runs
	int (*claims)(str base);
	// rewrites the script in place for storage, returns its length
	size_t (*store)(struct src* s, char* body, size_t len, size_t code_off);
	// fills argv to check the syntax of a script read from stdin, returns the program to run
	const char* (*check_argv)(const struct src* s, char** argv, size_t cap);
	// fills s->exec_pre and s->exec_suf, the argv ninit puts around the script
	void (*exec_args)(struct src* s);
	// the reason the #! arguments keep the script from running as stored, with the word
	const char* (*bad_arg)(const struct strv* a, const char** word);
};

extern const struct lang lang_shell, lang_python, lang_lua, lang_perl;

extern int g_sh_lexed;

const struct lang* lang_select(struct src* s, const char* fname, str body);
int sh_lexed(const char* argv0);
size_t blank_header(struct src* s, char* body, size_t len, size_t code_off);
void late_directive(const char* fname, str line);
int claims_versioned(str base, str name);
char* escape_name(str head, const char* name, char q, str tail);
void check_exec_args(const struct src* s, const char* fname);
