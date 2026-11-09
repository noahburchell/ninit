#include "parser.h"
#include "../util.h"
#include "../../src/ngraph.h"

#include <stdio.h>
#include <string.h>

bool g_sh_lexed = true;

// the stripper follows sh lexing, under zsh it would cut glob flags such as (#i)
bool sh_lexed(const char* argv0) {
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
			return true;
		}
	}

	return false;
}

static bool word_sep(char c) {
	return c == ' ' || c == '\t' || c == '\r';
}

// the next word of S, S keeps the text after it
static str word_at(str* s) {
	size_t k = 0, e;
	str w;

	while(k < s->n && word_sep(s->p[k])) {
		k++;
	}

	for(e = k; e < s->n && !word_sep(s->p[e]); e++)
		;
	w = (str){ s->p + k, e - k };
	s->p += e;
	s->n -= e;

	return w;
}

static str rtrim(str s) {
	while(s.n && word_sep(s.p[s.n - 1])) {
		s.n--;
	}

	return s;
}

struct shebang {
	str prog;
	str rest;
};

enum env_kind {
	ENV_PROG,
	ENV_SKIP,
	ENV_ARG,
	ENV_SPLIT,
	ENV_END
};

// what a word of an env command line is, as gnu and busybox env parse it
static enum env_kind env_word(str w, bool opts, size_t* off) {
	static const str takes[] = { STR_INIT("argv0"), STR_INIT("chdir"), STR_INIT("unset") };
	const char* eq;
	str name;

	if(!opts || w.p[0] != '-') {
		return memchr(w.p, '=', w.n) ? ENV_SKIP : ENV_PROG;
	}

	if(w.n == 1) {
		return ENV_SKIP;
	}

	if(w.p[1] != '-') {
		for(size_t k = 1; k < w.n; k++) {
			// the -S argument is split into words that env parses again
			if(w.p[k] == 'S') {
				*off = k + 1;
				return ENV_SPLIT;
			}

			if(w.p[k] == 'a' || w.p[k] == 'C' || w.p[k] == 'u') {
				return k + 1 == w.n ? ENV_ARG : ENV_SKIP;
			}
		}

		return ENV_SKIP;
	}

	eq = memchr(w.p, '=', w.n);
	name = (str){ w.p + 2, (eq ? (size_t)(eq - w.p) : w.n) - 2 };
	if(!name.n) {
		return ENV_END;
	}

	// getopt_long takes any unambiguous prefix of a long option
	if(str_prefix(STR("split-string"), name)) {
		*off = eq ? (size_t)(eq - w.p) + 1 : w.n;
		return ENV_SPLIT;
	}

	for(size_t k = 0; !eq && k < sizeof(takes) / sizeof(*takes); k++) {
		if(str_prefix(takes[k], name)) {
			return ENV_ARG;
		}
	}

	return ENV_SKIP;
}

// the program a #! line runs and what follows it, looking through env and its options
static bool parse_shebang(str line, struct shebang* sb) {
	str rest = { line.p + 2, line.n - 2 }, w = word_at(&rest);
	size_t off;
	bool opts = true;
	enum env_kind what;

	if(!w.n) {
		return false;
	}

	if(str_eq(str_base(w), STR("env"))) {
		for(w = word_at(&rest); w.n; w = word_at(&rest)) {
			what = env_word(w, opts, &off);
			while(what == ENV_SPLIT && off < w.n) {
				w.p += off;
				w.n -= off;
				what = env_word(w, opts, &off);
			}

			if(what == ENV_PROG) {
				break;
			}

			if(what == ENV_END) {
				opts = false;
			} else if(what == ENV_ARG && !word_at(&rest).n) {
				return false;
			}
		}

		if(!w.n) {
			return false;
		}
	}

	sb->prog = w;
	sb->rest = rest;

	return true;
}

// the basename of the program a #! line names, through env and its options
static str shebang_interp(str line) {
	struct shebang sb;

	if(!parse_shebang(line, &sb)) {
		return (str){ 0 };
	}

	return str_base(sb.prog);
}

static bool is_name(str w, const char* path) {
	return str_eq(w, str_c(path_base(path)));
}

// the shell runs every service, a #! line naming anything else is a comment worth a warning
void shebang_warn(const char* fname, str body) {
	str line, w;

	if(!str_prefix(body, STR("#!"))) {
		return;
	}

	line = str_cut(&body, '\n');
	w = shebang_interp(line);
	if(!w.n || is_name(w, NG_SHELL) || is_name(w, NG_SHELL_ARGV0) || (g_sh_lexed && is_name(w, "sh"))) {
		return;
	}

	line = rtrim(line);
	fprintf(stderr, "ninitctl: warning: %s/%s: '%.*s' is ignored, the script runs under %s\n",
		g_dir, fname, (int)line.n, line.p, NG_SHELL);
}

// the characters of a word the shell passes through unchanged, no expansion, no quoting
static bool plain_char(char c) {
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
			if(*w == '-' || n > NG_MAX_CMD || (*w != '/' && memchr(w, '/', n))) {
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

size_t shell_store(struct src* s, char* body, size_t len, size_t code_off) {
	if(!g_sh_lexed) {
		return len;
	}

	s->stripped = true;
	len = compact(body, len, code_off, s->name);
	detect_direct(s, body);

	return len;
}

// the argv of the syntax check, which reads the script from stdin
void shell_check_argv(char** argv) {
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
}
