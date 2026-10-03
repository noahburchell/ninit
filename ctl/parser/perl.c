#include "lang.h"
#include "parser.h"
#include "../util.h"

#include <stdint.h>
#include <string.h>

static int perl_claims(const char *base, size_t len)
{
	return claims_versioned(base, len, "perl");
}

// perl -c runs BEGIN blocks and use statements, there is no check that runs nothing
static const char *perl_check_argv(const struct src *s, char **argv, size_t cap)
{
	static char check[] = "-c", stdin_prog[] = "-";

	argv[0] = (char *)(uintptr_t)s->interp;
	argv[1] = check;
	argv[2] = stdin_prog;
	argv[3] = NULL;
	return s->interp;
}

// a first line flushes stdout per line for the log and names the script, #line restores line 1
static void perl_exec_args(struct src *s)
{
	static char dashe[] = "-e", line1[] = "#line 1";
	static const char head[] = "$| = 1; $0 = '";
	char *pre = xmalloc(sizeof(head) + 2 * strlen(s->name) + 4), *p = pre;

	memcpy(p, head, sizeof(head) - 1);
	p += sizeof(head) - 1;
	for (const char *c = s->name; *c; c++) {
		if (*c == '\'' || *c == '\\')
			*p++ = '\\';
		*p++ = *c;
	}
	memcpy(p, "';", 3);

	strv_push(&s->exec_pre, (char *)(uintptr_t)s->interp);
	for (uint32_t k = 0; k < s->iargs.n; k++)
		strv_push(&s->exec_pre, s->iargs.v[k]);
	strv_push(&s->exec_pre, dashe);
	strv_push(&s->exec_pre, pre);
	strv_push(&s->exec_pre, dashe);
	strv_push(&s->exec_pre, line1);
	strv_push(&s->exec_pre, dashe);
}

const struct lang lang_perl = {
	.name = "perl",
	.claims = perl_claims,
	.store = blank_header,
	.check_argv = perl_check_argv,
	.exec_args = perl_exec_args,
};
