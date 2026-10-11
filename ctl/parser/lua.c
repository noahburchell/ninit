#include "lang.h"
#include "parser.h"
#include "../util.h"

#include <stdint.h>
#include <string.h>

static int lua_claims(const char *base, size_t len)
{
	return claims_versioned(base, len, "lua") || (len >= 6 && !memcmp(base, "luajit", 6));
}

static int lua_blank(char c)
{
	return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

// the level of a long bracket opening at s[i], plus one, or 0 when s[i] opens none
static size_t lua_sep(const char *s, size_t i)
{
	size_t k = i + 1;

	while (s[k] == '=')
		k++;
	return s[k] == '[' ? k - i : 0;
}

// after the long bracket that closes level LV, from i, SIZE_MAX when none does
static size_t lua_close(const char *s, size_t i, size_t n, size_t lv)
{
	for (; i < n; i++) {
		size_t k = i + 1;

		if (s[i] != ']')
			continue;
		while (k < n && s[k] == '=')
			k++;
		if (k < n && s[k] == ']' && k - i - 1 == lv)
			return k + 1;
	}
	return SIZE_MAX;
}

static size_t lua_str(const char *s, size_t i, size_t n)
{
	char q = s[i++];

	while (i < n) {
		char c = s[i];

		if (c == q)
			return i + 1;
		if (c == '\n' || c == '\r')
			return SIZE_MAX;
		if (c != '\\') {
			i++;
		} else if (s[i + 1] == 'z') {
			for (i += 2; i < n && (lua_blank(s[i]) || s[i] == '\n' || s[i] == '\r'); i++)
				;
		} else {
			i += (s[i + 1] == '\r' && s[i + 2] == '\n') || (s[i + 1] == '\n' && s[i + 2] == '\r') ? 3 : 2;
		}
	}
	return SIZE_MAX;
}

// lua counts a lone carriage return as a line, and \n\r as one line where they meet
static size_t lua_strip(char *s, size_t n, size_t code)
{
	size_t i = code, j = code, e, lv;
	int pend = 0, crlf = 0;

	while (i < n) {
		int bol = 1;

		pend = 0;
		while (i < n && lua_blank(s[i]))
			i++;
		while (i < n) {
			char c = s[i];

			if (c == '\n') {
				crlf = i > code && s[i - 1] == '\r';
				s[j++] = '\n';
				i++;
				break;
			}
			if (lua_blank(c)) {
				pend = !bol;
				i++;
				continue;
			}
			if (c == '\r') {
				if (s[i + 1] == '\n') {
					i++;
					continue;
				}
				// a newline written before it pairs with it only where they met in the file
				if (j > code && s[j - 1] == '\n') {
					if (s[i - 1] != '\n') {
						s[j++] = ' ';
					} else if (crlf) {
						s[j - 1] = '\r';
						s[j++] = '\n';
					}
				}
				goto bail;
			}
			if (c == '-' && s[i + 1] == '-') {
				if (s[i + 2] == '[' && (lv = lua_sep(s, i + 2))) {
					size_t nl = 0;

					e = lua_close(s, i + 2 + lv + 1, n, lv - 1);
					if (e == SIZE_MAX || memchr(s + i, '\r', e - i))
						goto bail;
					for (size_t k = i; k < e; k++)
						nl += s[k] == '\n';
					// a comment separates the words around it, as its newlines do
					if (nl) {
						memset(s + j, '\n', nl);
						j += nl;
						bol = 1;
						pend = 0;
					} else {
						pend = !bol;
					}
					i = e;
					continue;
				}
				for (e = i; e < n && s[e] != '\n'; e++)
					if (s[e] == '\r' && s[e + 1] != '\n')
						goto bail;
				pend = 0;
				i = e;
				continue;
			}
			if (pend)
				s[j++] = ' ';
			pend = bol = 0;
			e = i + 1;
			if (c == '\'' || c == '"')
				e = lua_str(s, i, n);
			else if (c == '[' && (lv = lua_sep(s, i)))
				e = lua_close(s, i + lv + 1, n, lv - 1);
			if (e == SIZE_MAX)
				goto bail;
			memmove(s + j, s + i, e - i);
			j += e - i;
			i = e;
		}
	}
	s[j] = '\0';
	return j;

bail:
	// the blank before verbatim text keeps the words apart, - and -- would make a comment
	if (pend)
		s[j++] = ' ';
	memmove(s + j, s + i, n - i + 1);
	return j + n - i;
}

static size_t lua_store(struct src *s, char *body, size_t len, size_t code_off)
{
	size_t n = blank_header(s, body, len, code_off);

	s->stripped = 1;
	return lua_strip(body, n, n - (len - code_off));
}

static const char *lua_check_argv(const struct src *s, char **argv, size_t cap)
{
	static char dashe[] = "-e";
	// load() compiles the chunk without running it, loadstring is its lua 5.1 name
	static char prog[] = "local f, e = (loadstring or load)(io.read('*a'), '=lua') "
			     "if not f then io.stderr:write(e, '\\n') os.exit(1) end";

	argv[0] = (char *)(uintptr_t)s->interp;
	argv[1] = dashe;
	argv[2] = prog;
	argv[3] = NULL;
	return s->interp;
}

// lua -e names no script, so a first chunk sets arg[0] and flushes stdout per line for the log,
// exec_suf stays empty since lua would run a word after the last chunk as a script file
static void lua_exec_args(struct src *s)
{
	static const char head[] = "io.stdout:setvbuf(\"line\") arg = { [0] = \"";
	char *pre = xmalloc(sizeof(head) + 2 * strlen(s->name) + 4), *p = pre;

	memcpy(p, head, sizeof(head) - 1);
	p += sizeof(head) - 1;
	for (const char *c = s->name; *c; c++) {
		if (*c == '"' || *c == '\\')
			*p++ = '\\';
		*p++ = *c;
	}
	memcpy(p, "\" }", 4);

	strv_push(&s->exec_pre, s->interp);
	for (uint32_t k = 0; k < s->iargs.n; k++)
		strv_push(&s->exec_pre, s->iargs.v[k]);
	strv_push(&s->exec_pre, "-e");
	strv_push(&s->exec_pre, pre);
	strv_push(&s->exec_pre, "-e");
}

// lua takes no option clusters, and luajit -b compiles the arguments that follow it
static const char *lua_bad_arg(const struct strv *a, const char **word)
{
	for (uint32_t k = 0; k < a->n; k++) {
		const char *w = *word = a->v[k];

		if (w[0] != '-' || !w[1] || !strcmp(w, "--") || w[1] == 'b')
			return ARG_SCRIPT;
		if (w[1] == 'e')
			return ARG_CODE;
		// -l or -j last takes the -e after it as its argument, and lua exits with an error
		if ((w[1] == 'l' || w[1] == 'j') && !w[2] && ++k == a->n)
			return ARG_EXIT;
	}
	return NULL;
}

const struct lang lang_lua = {
	.name = "lua",
	.claims = lua_claims,
	.store = lua_store,
	.check_argv = lua_check_argv,
	.exec_args = lua_exec_args,
	.bad_arg = lua_bad_arg,
};
