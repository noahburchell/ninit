#include "lang.h"
#include "parser.h"
#include "../util.h"
#include "../../src/ngraph.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int word_sep(char c) {
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

enum {
	ENV_PROG,
	ENV_SKIP,
	ENV_ARG,
	ENV_SPLIT,
	ENV_END
};

// what a word of an env command line is, as gnu and busybox env parse it
static int env_word(str w, int opts, size_t* off) {
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
static int parse_shebang(str line, struct shebang* sb) {
	str rest = { line.p + 2, line.n - 2 }, w = word_at(&rest);
	size_t off;
	int opts = 1, what;

	if(!w.n) {
		return 0;
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
				opts = 0;
			} else if(what == ENV_ARG && !word_at(&rest).n) {
				return 0;
			}
		}

		if(!w.n) {
			return 0;
		}
	}

	sb->prog = w;
	sb->rest = rest;

	return 1;
}

// the basename of the interpreter a #! line names, through env and its options
static str shebang_interp(str line) {
	struct shebang sb;

	if(!parse_shebang(line, &sb)) {
		return (str){ 0 };
	}

	return str_base(sb.prog);
}

static int is_name(str w, const char* path) {
	return str_eq(w, str_c(path_base(path)));
}

static int runnable(const char* path) {
	struct stat st;

	return sys_stat(path, &st) == 0 && S_ISREG(st.st_mode) && sys_exec_ok(path, &st) == 0;
}

// DIR/PROG with DIR canonical, /usr/sbin is often a link to /usr/bin, PROG keeps its own
// name since python-exec picks the interpreter by the name it was run as
static char* found_in(str dir, str prog) {
	char d[NG_MAX_INTERP + 1];
	char* real;
	size_t rl;

	memcpy(d, dir.p, dir.n);
	d[dir.n] = '\0';
	real = sys_realpath(d);
	if(!real) {
		return NULL;
	}

	rl = strlen(real);
	if(rl + 1 + prog.n > NG_MAX_INTERP) {
		free(real);
		return NULL;
	}

	real = xrealloc(real, rl + 1 + prog.n + 1);
	real[rl] = '/';
	memcpy(real + rl + 1, prog.p, prog.n);
	real[rl + 1 + prog.n] = '\0';

	return real;
}

// the interpreter as an absolute path, an env lookup happens now so ninit runs it directly
static char* resolve(const char* fname, str prog) {
	char path[NG_MAX_INTERP + 1];
	str dirs = str_c(&NG_PATH[5]);
	struct stat st;

	if(memchr(prog.p, '/', prog.n)) {
		if(prog.p[0] != '/') {
			die("%s/%s: '%.*s' is not an absolute path", g_dir, fname, (int)prog.n, prog.p);
		}

		if(prog.n > NG_MAX_INTERP) {
			die("%s/%s: the interpreter path is %zu bytes, the maximum is %u",
			    g_dir, fname, prog.n, NG_MAX_INTERP);
		}

		memcpy(path, prog.p, prog.n);
		path[prog.n] = '\0';
		if(sys_stat(path, &st) < 0) {
			die("%s/%s: %s%s: %s", g_dir, fname, g_root_pfx, path, strerror(errno));
		}

		if(!S_ISREG(st.st_mode)) {
			die("%s/%s: %s%s is not a regular file", g_dir, fname, g_root_pfx, path);
		}

		if(sys_exec_ok(path, &st) < 0) {
			die("%s/%s: %s%s: %s", g_dir, fname, g_root_pfx, path, strerror(errno));
		}

		return xstrndup(path, prog.n);
	}

	while(dirs.n) {
		str d = str_cut(&dirs, ':');

		if(d.n + 1 + prog.n <= NG_MAX_INTERP) {
			memcpy(path, d.p, d.n);
			path[d.n] = '/';
			memcpy(path + d.n + 1, prog.p, prog.n);
			path[d.n + 1 + prog.n] = '\0';
			if(runnable(path)) {
				char* real = found_in(d, prog);

				return real ? real : xstrndup(path, d.n + 1 + prog.n);
			}
		}
	}

	die("%s/%s: '%.*s' is not in %s%s%s", g_dir, fname, (int)prog.n, prog.p, &NG_PATH[5],
	    g_root ? " below " : "", g_root ? g_root : "");
}

// the language a #! line selects, the configured shell for anything no language claims
const struct lang* lang_select(struct src* s, const char* fname, str body) {
	static const struct lang* const langs[] = { &lang_python, &lang_lua, &lang_perl };
	struct shebang sb;
	const char* why;
	const char* word;
	str line, w, a;

	s->interp = NG_SHELL;
	if(!str_prefix(body, STR("#!"))) {
		return &lang_shell;
	}

	line = str_cut(&body, '\n');
	w = shebang_interp(line);
	if(!w.n || is_name(w, NG_SHELL) || is_name(w, NG_SHELL_ARGV0)
	   || (g_sh_lexed && is_name(w, "sh"))) {
		return &lang_shell;
	}

	for(size_t k = 0; k < sizeof(langs) / sizeof(*langs); k++) {
		if(!langs[k]->claims(w)) {
			continue;
		}

		parse_shebang(line, &sb);
		// the kernel would pass these as one argument, they are split as env -S does
		while((a = word_at(&sb.rest)).n) {
			strv_push(&s->iargs, xstrndup(a.p, a.n));
		}

		why = langs[k]->bad_arg(&s->iargs, &word);
		if(why) {
			die("%s/%s: #! argument '%s' %s", g_dir, fname, word, why);
		}

		s->interp = resolve(fname, sb.prog);
		return langs[k];
	}

	line = rtrim(line);
	fprintf(stderr, "ninitctl: warning: %s/%s: '%.*s' is ignored, the script runs under %s\n",
		g_dir, fname, (int)line.n, line.p, NG_SHELL);

	return &lang_shell;
}

// header lines become empty lines, so line numbers in interpreter messages match the file
size_t blank_header(struct src* s, char* body, size_t len, size_t code_off) {
	size_t j = 0;

	for(size_t k = 0; k < code_off; k++) {
		if(body[k] == '\n') {
			body[j++] = '\n';
		}
	}

	memmove(body + j, body + code_off, len - code_off + 1);

	return len - code_off + j;
}

// a #% line after the header is a comment, the stripper of each language reports it
void late_directive(const char* fname, str line) {
	line = rtrim(line);
	fprintf(stderr, "ninitctl: warning: %s/%s: '%.*s' follows a command, treating it as a comment\n",
		g_dir, fname, (int)line.n, line.p);
}

// NAME followed by nothing but a version, such as python3.14 or lua5.4
int claims_versioned(str base, str name) {
	if(!str_prefix(base, name)) {
		return 0;
	}

	for(size_t k = name.n; k < base.n; k++) {
		if((base.p[k] < '0' || base.p[k] > '9') && base.p[k] != '.') {
			return 0;
		}
	}

	return 1;
}

// HEAD, NAME with Q and backslash escaped, then TAIL
char* escape_name(str head, const char* name, char q, str tail) {
	char* out = xmalloc(head.n + 2 * strlen(name) + tail.n + 1);
	char* p = out;

	memcpy(p, head.p, head.n);
	p += head.n;
	for(const char* c = name; *c; c++) {
		if(*c == q || *c == '\\') {
			*p++ = '\\';
		}

		*p++ = *c;
	}

	memcpy(p, tail.p, tail.n);
	p[tail.n] = '\0';

	return out;
}

// the arguments ninit passes must fit the graph format
void check_exec_args(const struct src* s, const char* fname) {
	if(s->exec_pre.n + s->exec_suf.n > NG_MAX_EXEC_ARGS) {
		die("%s/%s: %u interpreter arguments, the maximum is %u", g_dir, fname,
		    s->exec_pre.n + s->exec_suf.n, NG_MAX_EXEC_ARGS);
	}

	for(uint32_t k = 0; k < s->exec_pre.n + s->exec_suf.n; k++) {
		const char* a = k < s->exec_pre.n ? s->exec_pre.v[k] : s->exec_suf.v[k - s->exec_pre.n];

		if(strlen(a) > NG_MAX_EXEC_ARG) {
			die("%s/%s: an interpreter argument is %zu bytes, the maximum is %u",
			    g_dir, fname, strlen(a), NG_MAX_EXEC_ARG);
		}
	}
}
