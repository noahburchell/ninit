#include "lang.h"
#include "parser.h"
#include "../../src/ngraph.h"

#include <string.h>

int g_sh_lexed = 1;

// the stripper follows sh lexing, under zsh it would cut glob flags such as (#i)
int sh_lexed(const char* argv0) {
	static const char* const names[] = {
		"sh",
		"bash",
		"dash",
		"ash",
		"ksh",
		"mksh",
		"oksh",
		"loksh",
		"yash",
		"posh",
	};
	const char* base = path_base(argv0);

	for(size_t k = 0; k < sizeof(names) / sizeof(*names); k++) {
		if(!strcmp(base, names[k])) {
			return 1;
		}
	}

	return 0;
}

// the characters of a word the shell passes through unchanged, no expansion, no quoting
static int plain_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
	       || c == '_' || c == '.' || c == '/' || c == ':' || c == ',' || c == '=' || c == '+'
	       || c == '%' || c == '@' || c == '-';
}

// a stripped script that is one exec line of plain words runs as that execve, without the shell,
// anything the scan cannot vouch for keeps running under it and behaves the same
static void detect_direct(struct src* s, const char* body) {
	const char* p = body;
	const char* w;
	size_t n;

	while(*p == '\n') {
		p++;
	}

	if(strchr(p, '\n') || strncmp(p, "exec ", 5)) {
		return;
	}

	for(p += 5; *p; p += n) {
		while(*p == ' ') {
			p++;
		}

		for(n = 0; p[n] && p[n] != ' '; n++) {
			if(!plain_char(p[n])) {
				goto no;
			}
		}

		if(!n) {
			break;
		}

		w = p;
		if(!s->direct.n) {
			if(*w == '-' || n > NG_MAX_INTERP || (*w != '/' && memchr(w, '/', n))) {
				goto no;
			}
		} else if(n > NG_MAX_EXEC_ARG) {
			goto no;
		}

		if(s->direct.n == NG_MAX_EXEC_ARGS) {
			goto no;
		}

		strv_push(&s->direct, xstrndup(w, n));
	}

	if(s->direct.n) {
		return;
	}
no:
	s->direct.n = 0;
}

static size_t shell_store(struct src* s, char* body, size_t len, size_t code_off) {
	if(!g_sh_lexed) {
		return len;
	}

	s->stripped = 1;
	len = compact(body, len, code_off, s->name);
	detect_direct(s, body);

	return len;
}

static const char* shell_check_argv(const struct src* s, char** argv, size_t cap) {
	static char argv0[] = NG_SHELL_ARGV0, dashn[] = "-n", dasho[] = "-O", extglob[] = "extglob";
	size_t k = 0;

	argv[k++] = argv0;
	// -n never runs the shopt that turns extglob on, so bash would refuse its patterns
	if(!strcmp(path_base(NG_SHELL), "bash")) {
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
