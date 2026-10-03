#pragma once

#include "../util.h"
#include "lang.h"

#include <stddef.h>
#include <stdint.h>

struct src {
	char *name;
	const char *script;
	uint8_t type;
	int have_type;
	uint8_t onfail;
	int have_onfail;
	uint8_t restart;
	uint16_t notify;
	uint32_t start_ms;
	uint32_t stop_ms;
	uint16_t retry_ms;
	uint8_t start_tries;
	uint8_t pflags;
	struct strv depon;
	struct strv depof;
	const struct lang *lang;
	const char *interp;
	int stripped;
};

size_t compact(char *s, size_t n, size_t code_off, const char *fname);
void parse_src(struct src *s, const char *fname, char *body, size_t len);
