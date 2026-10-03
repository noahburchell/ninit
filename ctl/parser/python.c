#include "lang.h"
#include "parser.h"
#include "../util.h"

#include <stdint.h>

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

const struct lang lang_python = {
	.name = "python",
	.claims = python_claims,
	.store = blank_header,
	.check_argv = python_check_argv,
	.exec_args = python_exec_args,
};
