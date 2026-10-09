#include "lang.h"
#include "parser.h"
#include "../util.h"

#include <ctype.h>
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

// the reason one switch cluster is refused, *next is set when its argument is the next word
static const char *perl_cluster(const char *c, int *next)
{
	for (; *c; c++) {
		switch (*c) {
		case 'e':
		case 'E':
			return ARG_CODE;
		case 'x':
			return ARG_SCRIPT;
		case 'c':
		case 'h':
		case 'u':
		case 'v':
		case 'V':
			return ARG_EXIT;
		case 'a':
		case 'F':
		case 'n':
		case 'p':
			return ARG_LOOP;
		case 'I':
			*next = !c[1];
			return NULL;
		// without a module -d is the interactive debugger, reading /dev/null
		case 'd':
			c += c[1] == 't';
			return c[1] == ':' ? NULL : ARG_EXIT;
		case 'D':
		case 'i':
		case 'm':
		case 'M':
			return NULL;
		case '0':
			if (c[1] == 'x' || c[1] == 'X')
				for (c++; isxdigit((unsigned char)c[1]); c++)
					;
			else
				for (; c[1] >= '0' && c[1] <= '7'; c++)
					;
			break;
		case 'l':
			for (; c[1] >= '0' && c[1] <= '7'; c++)
				;
			break;
		case 'C':
			for (; c[1] && strchr("0123456789IOEioSALaD", c[1]); c++)
				;
			break;
		default:
			break;
		}
	}
	return NULL;
}

// perl takes the first word that is no switch as the script, -x discards a script without #!perl
static const char *perl_bad_arg(const struct strv *a, const char **word)
{
	for (uint32_t k = 0; k < a->n; k++) {
		const char *w = *word = a->v[k], *why;
		int next = 0;

		if (w[0] != '-' || !w[1] || !strcmp(w, "--"))
			return ARG_SCRIPT;
		why = perl_cluster(w + 1, &next);
		if (why)
			return why;
		if (next && ++k == a->n)
			return ARG_SCRIPT;
	}
	return NULL;
}

const struct lang lang_perl = {
	.name = "perl",
	.claims = perl_claims,
	.store = blank_header,
	.check_argv = perl_check_argv,
	.exec_args = perl_exec_args,
	.bad_arg = perl_bad_arg,
};
