#pragma once

#include "../util.h"
#include "../../help/str.h"

#include <stddef.h>
#include <stdint.h>

struct src {
	char*       name;
	const char* script;
	uint8_t     type;
	bool        have_type;
	uint8_t     onfail;
	bool        have_onfail;
	uint8_t     restart;
	uint16_t    notify;
	uint32_t    start_ms;
	uint32_t    stop_ms;
	uint16_t    retry_ms;
	uint8_t     start_tries;
	uint8_t     pflags;
	struct strv depon;
	struct strv depof;
	bool        stripped;
	// the argv of a bare exec line, empty when the script needs the shell
	struct strv direct;
};

extern bool g_sh_lexed;

size_t compact(char* s, size_t n, size_t code_off, const char* fname);
void parse_src(struct src* s, const char* fname, char* body, size_t len);
bool sh_lexed(const char* argv0);
void shebang_warn(const char* fname, str body);
size_t shell_store(struct src* s, char* body, size_t len, size_t code_off);
void shell_check_argv(char** argv);
