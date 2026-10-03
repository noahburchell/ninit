#include "lang.h"
#include "parser.h"
#include "../util.h"
#include "../../src/ngraph.h"

#include <stdio.h>
#include <string.h>

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

// the basename of the interpreter a #! line names, through env and its options
static const char *shebang_interp(const char *line, size_t n, size_t *len)
{
	const char *p = line + 2, *end = line + n, *w;
	size_t wl;

	w = word_at(&p, end, &wl);
	if (!w)
		return NULL;
	w = base_of(w, &wl);
	if (wl == 3 && !memcmp(w, "env", 3)) {
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
			return NULL;
		w = base_of(w, &wl);
	}
	*len = wl;
	return wl ? w : NULL;
}

static int is_name(const char *w, size_t len, const char *path)
{
	const char *b = strrchr(path, '/');

	b = b ? b + 1 : path;
	return strlen(b) == len && !memcmp(w, b, len);
}

// the language a #! line selects, the configured shell for anything it does not name
const struct lang *lang_select(struct src *s, const char *fname, const char *body)
{
	const char *nl, *w;
	size_t n, len;

	s->interp = NG_SHELL;
	if (body[0] != '#' || body[1] != '!')
		return &lang_shell;
	nl = strchr(body, '\n');
	n = nl ? (size_t)(nl - body) : strlen(body);
	w = shebang_interp(body, n, &len);
	if (!w || is_name(w, len, NG_SHELL) || is_name(w, len, NG_SHELL_ARGV0) ||
	    (g_sh_lexed && is_name(w, len, "sh")))
		return &lang_shell;
	while (n && (body[n - 1] == ' ' || body[n - 1] == '\t' || body[n - 1] == '\r'))
		n--;
	fprintf(stderr, "ninitctl: warning: %s/%s: '%.*s' is ignored, the script runs under %s\n",
		g_dir, fname, (int)n, body, NG_SHELL);
	return &lang_shell;
}
