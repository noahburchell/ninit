#include "lang.h"
#include "parser.h"
#include "../util.h"

#include <stdint.h>
#include <string.h>

static int lua_claims(const char *base, size_t len)
{
	return claims_versioned(base, len, "lua") || (len >= 6 && !memcmp(base, "luajit", 6));
}

static const char *lua_check_argv(const struct src *s, char **argv, size_t cap)
{
	static char dashe[] = "-e";
	// load() compiles the chunk without running it, loadstring is its lua 5.1 name
	static char prog[] = "local f, e = (loadstring or load)(io.read('*a'), '=lua') "
			     "if not f then io.stderr:write(e, '\\n') os.exit(1) end";

	argv[0] = (char *)(uintptr_t)s->interp;
	argv[1] = dashe;
	argv[2] = prog;
	argv[3] = NULL;
	return s->interp;
}

// lua -e names no script, so a first chunk sets arg[0] and flushes stdout per line for the log
static void lua_exec_args(struct src *s)
{
	static char dashe[] = "-e";
	static const char head[] = "io.stdout:setvbuf(\"line\") arg = { [0] = \"";
	char *pre = xmalloc(sizeof(head) + 2 * strlen(s->name) + 4), *p = pre;

	memcpy(p, head, sizeof(head) - 1);
	p += sizeof(head) - 1;
	for (const char *c = s->name; *c; c++) {
		if (*c == '"' || *c == '\\')
			*p++ = '\\';
		*p++ = *c;
	}
	memcpy(p, "\" }", 4);

	strv_push(&s->exec_pre, (char *)(uintptr_t)s->interp);
	for (uint32_t k = 0; k < s->iargs.n; k++)
		strv_push(&s->exec_pre, s->iargs.v[k]);
	strv_push(&s->exec_pre, dashe);
	strv_push(&s->exec_pre, pre);
	strv_push(&s->exec_pre, dashe);
}

const struct lang lang_lua = {
	.name = "lua",
	.claims = lua_claims,
	.store = blank_header,
	.check_argv = lua_check_argv,
	.exec_args = lua_exec_args,
};
