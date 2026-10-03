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

static const char *word_at(const char **p, const char *end, size_t *len)
{
	const char *w;

	while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r'))
		(*p)++;
	w = *p;
	while (*p < end && **p != ' ' && **p != '\t' && **p != '\r')
		(*p)++;
	*len = (size_t)(*p - w);
	return *len ? w : NULL;
}

static const char *base_of(const char *w, size_t *len)
{
	const char *b = w;

	for (size_t k = 0; k < *len; k++)
		if (w[k] == '/')
			b = w + k + 1;
	*len -= (size_t)(b - w);
	return b;
}

struct shebang {
	const char *prog;
	size_t len;
	const char *rest;
	const char *end;
};

// the program a #! line runs and what follows it, looking through env and its options
static int parse_shebang(const char *line, size_t n, struct shebang *sb)
{
	const char *p = line + 2, *end = line + n, *w, *b;
	size_t wl, bl;

	w = word_at(&p, end, &wl);
	if (!w)
		return 0;
	bl = wl;
	b = base_of(w, &bl);
	if (bl == 3 && !memcmp(b, "env", 3)) {
		while ((w = word_at(&p, end, &wl))) {
			if (wl > 2 && w[0] == '-' && w[1] == 'S') {
				w += 2;
				wl -= 2;
				break;
			}
			if (w[0] == '-') {
				if (wl == 2 && (w[1] == 'u' || w[1] == 'C'))
					word_at(&p, end, &wl);
				continue;
			}
			if (!memchr(w, '=', wl))
				break;
		}
		if (!w)
			return 0;
	}
	sb->prog = w;
	sb->len = wl;
	sb->rest = p;
	sb->end = end;
	return 1;
}

// the basename of the interpreter a #! line names, through env and its options
static const char *shebang_interp(const char *line, size_t n, size_t *len)
{
	struct shebang sb;
	const char *b;

	if (!parse_shebang(line, n, &sb))
		return NULL;
	*len = sb.len;
	b = base_of(sb.prog, len);
	return *len ? b : NULL;
}

static int is_name(const char *w, size_t len, const char *path)
{
	const char *b = strrchr(path, '/');

	b = b ? b + 1 : path;
	return strlen(b) == len && !memcmp(w, b, len);
}

static int runnable(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

// DIR/PROG with DIR canonical, /usr/sbin is often a link to /usr/bin, PROG keeps its own
// name since python-exec picks the interpreter by the name it was run as
static char *found_in(const char *dir, size_t dl, const struct shebang *sb)
{
	char d[NG_MAX_INTERP + 1], *real;
	size_t rl;

	memcpy(d, dir, dl);
	d[dl] = '\0';
	real = realpath(d, NULL);
	if (!real)
		return NULL;
	rl = strlen(real);
	if (rl + 1 + sb->len > NG_MAX_INTERP) {
		free(real);
		return NULL;
	}
	real = xrealloc(real, rl + 1 + sb->len + 1);
	real[rl] = '/';
	memcpy(real + rl + 1, sb->prog, sb->len);
	real[rl + 1 + sb->len] = '\0';
	return real;
}

// the interpreter as an absolute path, an env lookup happens now so ninit runs it directly
static char *resolve(const char *fname, const struct shebang *sb)
{
	char path[NG_MAX_INTERP + 1];
	struct stat st;

	if (memchr(sb->prog, '/', sb->len)) {
		if (sb->prog[0] != '/')
			die("%s/%s: '%.*s' is not an absolute path", g_dir, fname, (int)sb->len, sb->prog);
		if (sb->len > NG_MAX_INTERP)
			die("%s/%s: the interpreter path is %zu bytes, the maximum is %u",
			    g_dir, fname, sb->len, NG_MAX_INTERP);
		memcpy(path, sb->prog, sb->len);
		path[sb->len] = '\0';
		if (stat(path, &st) < 0)
			die("%s/%s: %s: %s", g_dir, fname, path, strerror(errno));
		if (!S_ISREG(st.st_mode))
			die("%s/%s: %s is not a regular file", g_dir, fname, path);
		if (access(path, X_OK) < 0)
			die("%s/%s: %s: %s", g_dir, fname, path, strerror(errno));
		return xstrndup(path, sb->len);
	}
	for (const char *d = &NG_PATH[5]; *d;) {
		const char *c = strchr(d, ':');
		size_t dl = c ? (size_t)(c - d) : strlen(d);

		if (dl + 1 + sb->len <= NG_MAX_INTERP) {
			memcpy(path, d, dl);
			path[dl] = '/';
			memcpy(path + dl + 1, sb->prog, sb->len);
			path[dl + 1 + sb->len] = '\0';
			if (runnable(path)) {
				char *real = found_in(d, dl, sb);

				return real ? real : xstrndup(path, dl + 1 + sb->len);
			}
		}
		d += dl + (c != NULL);
	}
	die("%s/%s: '%.*s' is not in %s", g_dir, fname, (int)sb->len, sb->prog, &NG_PATH[5]);
}

// the language a #! line selects, the configured shell for anything no language claims
const struct lang *lang_select(struct src *s, const char *fname, const char *body)
{
	static const struct lang *const langs[] = { &lang_python, &lang_lua, &lang_perl };
	struct shebang sb;
	const char *nl, *w, *p, *a;
	size_t n, len, al;

	s->interp = NG_SHELL;
	if (body[0] != '#' || body[1] != '!')
		return &lang_shell;
	nl = strchr(body, '\n');
	n = nl ? (size_t)(nl - body) : strlen(body);
	w = shebang_interp(body, n, &len);
	if (!w || is_name(w, len, NG_SHELL) || is_name(w, len, NG_SHELL_ARGV0) ||
	    (g_sh_lexed && is_name(w, len, "sh")))
		return &lang_shell;
	for (size_t k = 0; k < sizeof(langs) / sizeof(*langs); k++) {
		if (!langs[k]->claims(w, len))
			continue;
		parse_shebang(body, n, &sb);
		s->interp = resolve(fname, &sb);
		// the kernel would pass these as one argument, they are split as env -S does
		for (p = sb.rest; (a = word_at(&p, sb.end, &al));)
			strv_push(&s->iargs, xstrndup(a, al));
		return langs[k];
	}
	while (n && (body[n - 1] == ' ' || body[n - 1] == '\t' || body[n - 1] == '\r'))
		n--;
	fprintf(stderr, "ninitctl: warning: %s/%s: '%.*s' is ignored, the script runs under %s\n",
		g_dir, fname, (int)n, body, NG_SHELL);
	return &lang_shell;
}

// header lines become empty lines, so line numbers in interpreter messages match the file
size_t blank_header(struct src *s, char *body, size_t len, size_t code_off)
{
	size_t j = 0;

	for (size_t k = 0; k < code_off; k++)
		if (body[k] == '\n')
			body[j++] = '\n';
	memmove(body + j, body + code_off, len - code_off + 1);
	return len - code_off + j;
}

// NAME followed by nothing but a version, such as python3.14 or lua5.4
int claims_versioned(const char *base, size_t len, const char *name)
{
	size_t nl = strlen(name);

	if (len < nl || memcmp(base, name, nl))
		return 0;
	for (size_t k = nl; k < len; k++)
		if ((base[k] < '0' || base[k] > '9') && base[k] != '.')
			return 0;
	return 1;
}

// the arguments ninit passes must fit the graph format
void check_exec_args(const struct src *s, const char *fname)
{
	if (s->exec_pre.n + s->exec_suf.n > NG_MAX_EXEC_ARGS)
		die("%s/%s: %u interpreter arguments, the maximum is %u", g_dir, fname,
		    s->exec_pre.n + s->exec_suf.n, NG_MAX_EXEC_ARGS);
	for (uint32_t k = 0; k < s->exec_pre.n + s->exec_suf.n; k++) {
		const char *a = k < s->exec_pre.n ? s->exec_pre.v[k] : s->exec_suf.v[k - s->exec_pre.n];

		if (strlen(a) > NG_MAX_EXEC_ARG)
			die("%s/%s: an interpreter argument is %zu bytes, the maximum is %u",
			    g_dir, fname, strlen(a), NG_MAX_EXEC_ARG);
	}
}
