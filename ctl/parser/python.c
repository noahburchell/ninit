#include "lang.h"
#include "parser.h"
#include "../util.h"

#include <stdint.h>
#include <string.h>

static int python_claims(const char *base, size_t len)
{
	return claims_versioned(base, len, "python");
}

static const char *python_check_argv(const struct src *s, char **argv, size_t cap)
{
	static char isolated[] = "-I", nocache[] = "-B", dashc[] = "-c";
	// compile() checks the syntax without running any of the script
	static char prog[] = "import sys\n"
			     "try:\n"
			     "    compile(sys.stdin.buffer.read(), 'python', 'exec')\n"
			     "except SyntaxError as e:\n"
			     "    sys.exit('python: line %d: %s' % (e.lineno, e.msg))\n";

	argv[0] = (char *)(uintptr_t)s->interp;
	argv[1] = isolated;
	argv[2] = nocache;
	argv[3] = dashc;
	argv[4] = prog;
	argv[5] = NULL;
	return s->interp;
}

// -I keeps / off sys.path, -B writes no __pycache__ as root, -u keeps output in step with the log
static void python_exec_args(struct src *s)
{
	static char isolated[] = "-I", nocache[] = "-B", unbuffered[] = "-u", dashc[] = "-c";

	strv_push(&s->exec_pre, (char *)(uintptr_t)s->interp);
	strv_push(&s->exec_pre, isolated);
	strv_push(&s->exec_pre, nocache);
	strv_push(&s->exec_pre, unbuffered);
	for (uint32_t k = 0; k < s->iargs.n; k++)
		strv_push(&s->exec_pre, s->iargs.v[k]);
	strv_push(&s->exec_pre, dashc);
	strv_push(&s->exec_suf, s->name);
}

// python stops at the first word that is no option and runs it as the script
static const char *python_bad_arg(const struct strv *a, const char **word)
{
	for (uint32_t k = 0; k < a->n; k++) {
		const char *w = *word = a->v[k];

		if (w[0] != '-' || !w[1] || !strcmp(w, "--"))
			return ARG_SCRIPT;
		if (w[1] == '-') {
			if (!strncmp(w, "--help", 6) || !strcmp(w, "--version"))
				return ARG_EXIT;
			if (!strcmp(w, "--check-hash-based-pycs") && ++k == a->n)
				return ARG_SCRIPT;
			continue;
		}
		for (const char *c = w + 1; *c; c++) {
			if (*c == 'c' || *c == 'm')
				return ARG_SCRIPT;
			if (*c == 'h' || *c == '?' || *c == 'V')
				return ARG_EXIT;
			// the option argument is the rest of the word or the next word
			if (*c == 'W' || *c == 'X') {
				if (!c[1] && ++k == a->n)
					return ARG_SCRIPT;
				break;
			}
		}
	}
	return NULL;
}

const struct lang lang_python = {
	.name = "python",
	.claims = python_claims,
	.store = blank_header,
	.check_argv = python_check_argv,
	.exec_args = python_exec_args,
	.bad_arg = python_bad_arg,
};
