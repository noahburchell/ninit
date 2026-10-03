#include "lang.h"
#include "parser.h"
#include "../../src/ngraph.h"

#include <string.h>

int g_sh_lexed = 1;

// the stripper follows sh lexing, under zsh it would cut glob flags such as (#i)
int sh_lexed(const char *argv0)
{
	static const char *const names[] = {
		"sh", "bash", "dash", "ash", "ksh", "mksh", "oksh", "loksh", "yash", "posh",
	};
	const char *base = strrchr(argv0, '/');

	base = base ? base + 1 : argv0;
	for (size_t k = 0; k < sizeof(names) / sizeof(*names); k++)
		if (!strcmp(base, names[k]))
			return 1;
	return 0;
}

static size_t shell_store(struct src *s, char *body, size_t len, size_t code_off)
{
	if (!g_sh_lexed)
		return len;
	s->stripped = 1;
	return compact(body, len, code_off, s->name);
}

static const char *shell_check_argv(const struct src *s, char **argv, size_t cap)
{
	static char argv0[] = NG_SHELL_ARGV0, dashn[] = "-n", dasho[] = "-O", extglob[] = "extglob";
	size_t k = 0;

	argv[k++] = argv0;
	// -n never runs the shopt that turns extglob on, so bash would refuse its patterns
	if (!strcmp(strrchr(NG_SHELL, '/') + 1, "bash")) {
		argv[k++] = dasho;
		argv[k++] = extglob;
	}
	argv[k++] = dashn;
	argv[k] = NULL;
	return NG_SHELL;
}

const struct lang lang_shell = {
	.name = "shell",
	.store = shell_store,
	.check_argv = shell_check_argv,
};
