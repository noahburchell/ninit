#include "parser.h"
#include "../util.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CZ_DEPTH	32
#define CZ_TAG		128
#define CZ_HD		16
#define CZ_KW		1
#define CZ_NAME		2
#define CZ_TIME		4

struct cz_hd {
	uint32_t len;
	uint8_t dash;
	uint8_t quoted;
	char tag[CZ_TAG];
};

struct cz_out {
	char *o;
	size_t j, keep, code_off;
	const char *fname;
};

static size_t sk_cmd(const char *s, size_t i, size_t n, int d, struct cz_out *e, int nested);
static size_t sk_dq(const char *s, size_t i, size_t n, int d);
static size_t sk_brace(const char *s, size_t i, size_t n, int d);
static size_t sk_pair(const char *s, size_t i, size_t n, int d, char open, char close, unsigned depth);

static int cz_meta(char c)
{
	switch (c) {
	case ';':
	case '&':
	case '|':
	case '(':
	case ')':
	case '<':
	case '>':
		return 1;
	}
	return 0;
}

static int cz_delim(char c)
{
	return !c || c == ' ' || c == '\t' || c == '\n' || cz_meta(c);
}

static size_t cz_ident(const char *s)
{
	size_t l = 0;

	if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_'))
		return 0;
	while ((s[l] >= 'a' && s[l] <= 'z') || (s[l] >= 'A' && s[l] <= 'Z') ||
	       (s[l] >= '0' && s[l] <= '9') || s[l] == '_')
		l++;
	return l;
}

// CZ_NAME means the next word is a name and reserved words may follow it
static int cz_lead(const char *s, int rw)
{
	static const struct {
		char w[9];
		uint8_t rw;
	} kw[] = {
		{ "if", CZ_KW }, { "then", CZ_KW }, { "else", CZ_KW }, { "elif", CZ_KW },
		{ "fi", CZ_KW }, { "do", CZ_KW }, { "done", CZ_KW }, { "while", CZ_KW },
		{ "until", CZ_KW }, { "!", CZ_KW }, { "{", CZ_KW }, { "}", CZ_KW },
		{ "time", CZ_KW | CZ_TIME }, { "coproc", CZ_KW | CZ_NAME },
		{ "for", CZ_NAME }, { "select", CZ_NAME }, { "function", CZ_NAME },
	};
	size_t k, l;

	if (rw & CZ_TIME && s[0] == '-' && (s[1] == 'p' || s[1] == '-') && cz_delim(s[2]))
		return CZ_KW | CZ_TIME;
	for (k = 0; k < sizeof(kw) / sizeof(*kw); k++) {
		l = strlen(kw[k].w);
		if (!strncmp(s, kw[k].w, l) && cz_delim(s[l]))
			return kw[k].rw;
	}
	return 0;
}

static size_t sk_sq(const char *s, size_t i, size_t n)
{
	while (i < n && s[i] != '\'')
		i++;
	return i < n ? i + 1 : n;
}

static size_t sk_esc(const char *s, size_t i, size_t n, char end)
{
	while (i < n && s[i] != end)
		i += s[i] == '\\' ? 2 : 1;
	return i < n ? i + 1 : n;
}

static size_t sk_dollar(const char *s, size_t i, size_t n, int d, int dq)
{
	switch (s[i + 1]) {
	case '(':
		return s[i + 2] == '(' ? sk_pair(s, i + 3, n, d, '(', ')', 2) : sk_cmd(s, i + 2, n, d, NULL, 1);
	case '{':
		return sk_brace(s, i + 2, n, d);
	case '\'':
		return dq ? i + 1 : sk_esc(s, i + 2, n, '\'');
	case '"':
		return dq ? i + 1 : sk_dq(s, i + 2, n, d);
	}
	return i + 1;
}

static size_t sk_quoted(const char *s, size_t i, size_t n, int d, int dq)
{
	switch (s[i]) {
	case '\\':
		return i + 2;
	case '\'':
		return dq ? i + 1 : sk_sq(s, i + 1, n);
	case '"':
		return sk_dq(s, i + 1, n, d);
	case '`':
		return sk_esc(s, i + 1, n, '`');
	case '$':
		return sk_dollar(s, i, n, d, dq);
	}
	return i + 1;
}

static size_t sk_dq(const char *s, size_t i, size_t n, int d)
{
	if (++d > CZ_DEPTH)
		return n;
	while (i < n && s[i] != '"')
		i = sk_quoted(s, i, n, d, 1);
	return i < n ? i + 1 : n;
}

static size_t sk_brace(const char *s, size_t i, size_t n, int d)
{
	if (++d > CZ_DEPTH)
		return n;
	while (i < n && s[i] != '}')
		i = sk_quoted(s, i, n, d, 0);
	return i < n ? i + 1 : n;
}

static size_t sk_pair(const char *s, size_t i, size_t n, int d, char open, char close, unsigned depth)
{
	if (++d > CZ_DEPTH)
		return n;
	while (i < n) {
		if (s[i] == open) {
			depth++;
			i++;
		} else if (s[i] == close) {
			if (!--depth)
				return i + 1;
			i++;
		} else {
			i = sk_quoted(s, i, n, d, 0);
		}
	}
	return n;
}

static size_t sk_regex(const char *s, size_t i, size_t n, int d)
{
	while (s[i] == ' ' || s[i] == '\t')
		i++;
	while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\n')
		i = s[i] == '(' ? sk_pair(s, i + 1, n, d, '(', ')', 1) : sk_quoted(s, i, n, d, 0);
	return i < n ? i : n;
}

static size_t hd_tag(const char *s, size_t i, size_t n, struct cz_hd *h)
{
	uint32_t k = 0;

	i += 2;
	h->dash = s[i] == '-';
	h->quoted = 0;
	i += h->dash;
	while (s[i] == ' ' || s[i] == '\t')
		i++;
	while (i < n && !cz_delim(s[i])) {
		char c = s[i++];

		if (c == '$' || c == '`')
			return SIZE_MAX;
		if (c == '\'' || c == '"' || c == '\\')
			h->quoted = 1;
		if (c == '\'' || c == '"') {
			while (i < n && s[i] != c) {
				if (c == '"' && s[i] == '\\' && i + 1 < n)
					i++;
				if (k == CZ_TAG)
					return SIZE_MAX;
				h->tag[k++] = s[i++];
			}
			if (i++ >= n)
				return SIZE_MAX;
			continue;
		}
		if (c == '\\' && i < n)
			c = s[i++];
		if (k == CZ_TAG)
			return SIZE_MAX;
		h->tag[k++] = c;
	}
	if (!k || s[i] == '(' || s[i] == ')')
		return SIZE_MAX;
	h->len = k;
	return i;
}

static size_t hd_body(const char *s, size_t i, size_t n, const struct cz_hd *h, struct cz_out *e)
{
	while (i < n) {
		size_t b = i, p, eol, end, r, seg, m = 0;
		int last = 1, join;

		if (h->dash)
			while (b < n && s[b] == '\t')
				b++;
		for (p = b;; p = end) {
			const char *nl = memchr(s + p, '\n', n - p);

			eol = nl ? (size_t)(nl - s) : n;
			end = nl ? eol + 1 : n;
			for (r = eol; r > p && s[r - 1] == '\\'; r--)
				;
			join = !h->quoted && nl && ((eol - r) & 1);
			seg = eol - p - (size_t)join;
			if (last && (seg > h->len - m || memcmp(s + p, h->tag + m, seg)))
				last = 0;
			m += seg;
			if (!join)
				break;
		}
		last = last && m == h->len;
		if (e) {
			memmove(e->o + e->j, s + b, end - b);
			e->j += end - b;
			e->keep = last ? e->j - (end - eol) : e->j;
		}
		i = end;
		if (last)
			break;
	}
	return i;
}

static void cz_copy(struct cz_out *e, const char *s, size_t from, size_t to)
{
	if (!e)
		return;
	memmove(e->o + e->j, s + from, to - from);
	e->j += to - from;
	e->keep = e->j;
}

static size_t cz_sub(const char *s, size_t i, size_t n, int d, struct cz_out *e)
{
	cz_copy(e, s, i, i + 2);
	return sk_cmd(s, i + 2, n, d, e, 1);
}

static size_t sk_cmd(const char *s, size_t i, size_t n, int d, struct cz_out *e, int nested)
{
	struct cz_hd hd[CZ_HD];
	unsigned nhd = 0, ncs = 0, depth = 0, comp = 0, k;
	int ws = 1, bol = !nested, pend = 0, cmd = 1, cond = 0, eq = 0, tight = 0, rop = 0, after_eq;
	int rw = CZ_KW, pat = 0, cin = 0;
	size_t at = i, sub = SIZE_MAX;

	if (++d > CZ_DEPTH)
		goto bail;
	while (i < n) {
		char c = s[i];

		if (c == ' ' || c == '\t') {
			pend = !bol;
			ws = 1;
			tight = 0;
			i++;
			continue;
		}
		if (c == '\n') {
			if (e) {
				e->o[e->j++] = '\n';
				if (nhd)
					e->keep = e->j;
			}
			pend = tight = 0;
			ws = bol = cmd = 1;
			rw = CZ_KW;
			i++;
			for (k = 0; k < nhd; k++)
				i = hd_body(s, i, n, &hd[k], e);
			nhd = 0;
			continue;
		}
		if (c == '#' && ws) {
			const char *nl = memchr(s + i, '\n', n - i);
			size_t end = nl ? (size_t)(nl - s) : n;

			if (e && bol && s[i + 1] == '%' && i >= e->code_off) {
				size_t w = end;

				while (w > i && (s[w - 1] == ' ' || s[w - 1] == '\t' || s[w - 1] == '\r'))
					w--;
				fprintf(stderr, "ninitctl: warning: %s/%s: '%.*s' follows a command, "
					"treating it as a comment\n", g_dir, e->fname, (int)(w - i), s + i);
			}
			pend = tight = 0;
			i = end;
			continue;
		}

		at = i;
		if (c == '\\' && s[i + 1] == '\n' && tight)
			goto bail;
		tight = c != '\\' || s[i + 1] != '\n';
		if (e && pend)
			e->o[e->j++] = ' ';
		pend = bol = 0;
		after_eq = eq;
		eq = 0;
		if (ws && !rop && (!comp || depth != comp) && !cz_meta(c) && (c != '\\' || s[i + 1] != '\n')) {
			size_t l = cz_ident(s + i);
			int r = rw;

			rw = r & CZ_NAME ? CZ_KW : 0;
			if (cin) {
				if (cin == 2 && !strncmp(s + i, "in", 2) && cz_delim(s[i + 2])) {
					pat = 1;
					rw = CZ_KW;
				}
				cin = cin == 1 ? 2 : 0;
			} else if (r & CZ_KW && ncs && !strncmp(s + i, "esac", 4) && cz_delim(s[i + 4])) {
				ncs--;
				pat = 0;
				rw = CZ_KW;
			} else if (cond) {
				if (!strncmp(s + i, "]]", 2) && cz_delim(s[i + 2])) {
					cond = 0;
					rw = CZ_KW;
				}
			} else if (r & CZ_KW && !pat) {
				if (!strncmp(s + i, "case", 4) && cz_delim(s[i + 4])) {
					ncs++;
					cin = 1;
				} else if (!strncmp(s + i, "[[", 2) && cz_delim(s[i + 2])) {
					cond = 1;
				} else {
					rw |= cz_lead(s + i, r);
				}
			}
			if (cmd && l && s[i + l] == '[')
				sub = i + l;
			else if (!cmd || !l || (s[i + l] != '=' && (s[i + l] != '+' || s[i + l + 1] != '=')))
				cmd = rw & CZ_KW;
		}
		if (!cz_meta(c) && (c != '\\' || s[i + 1] != '\n'))
			rop = 0;

		switch (c) {
		case '\\':
			if (s[i + 1] != '\n')
				ws = 0;
			i += i + 1 < n ? 2 : 1;
			break;
		case '\'':
		case '"':
		case '`':
			i = sk_quoted(s, i, n, d, 0);
			ws = 0;
			break;
		case '$':
			if (s[i + 1] == '(' && s[i + 2] != '(') {
				i = cz_sub(s, i, n, d, e);
				ws = 0;
				continue;
			}
			i = sk_dollar(s, i, n, d, 0);
			ws = 0;
			break;
		case '(':
			if (s[i + 1] == '(') {
				i = sk_pair(s, i + 2, n, d, '(', ')', 2);
				ws = 1;
				cmd = 0;
				rw = CZ_KW;
				break;
			}
			i++;
			if (pat) {
				ws = 1;
				rw = 0;
				break;
			}
			depth++;
			if (after_eq && !ws)
				comp = depth;
			ws = cmd = 1;
			rw = CZ_KW;
			break;
		case ')':
			i++;
			ws = cmd = 1;
			rw = CZ_KW;
			if (pat) {
				pat = 0;
			} else if (depth) {
				if (depth == comp) {
					comp = 0;
					ws = rw = 0;
				}
				depth--;
			} else if (nested) {
				if (nhd)
					goto bail;
				cz_copy(e, s, at, i);
				return i;
			}
			break;
		case '<':
		case '>':
			rw = 0;
			if (s[i + 1] == '(' && s[i + 2] == '(') {
				i = sk_pair(s, i + 3, n, d, '(', ')', 2);
				ws = 0;
				break;
			}
			if (s[i + 1] == '(') {
				i = cz_sub(s, i, n, d, e);
				ws = 0;
				continue;
			}
			if (c == '<' && s[i + 1] == '<' && s[i + 2] == '<') {
				i += 3;
				ws = rop = 1;
				break;
			}
			if (c == '<' && s[i + 1] == '<') {
				if (nhd == CZ_HD)
					goto bail;
				i = hd_tag(s, i, n, &hd[nhd]);
				if (i == SIZE_MAX)
					goto bail;
				nhd++;
				ws = 0;
				break;
			}
			i++;
			ws = rop = 1;
			break;
		case ';':
		case '&':
		case '|':
			i++;
			if (!rop && (c != '&' || s[i] != '>')) {
				cmd = 1;
				rw = pat && c == '|' ? 0 : CZ_KW;
				if (c == ';' && ncs && (s[i] == ';' || s[i] == '&'))
					pat = 1;
			}
			ws = 1;
			tight = 0;
			break;
		default:
			if (i == sub) {
				i = sk_pair(s, i + 1, n, d, '[', ']', 1);
				sub = SIZE_MAX;
				cmd = s[i] == '=' || (s[i] == '+' && s[i + 1] == '=');
			} else if (c == '[' && ws && comp && depth == comp) {
				i = sk_pair(s, i + 1, n, d, '[', ']', 1);
			} else if (s[i + 1] == '(' && (c == '@' || c == '!' || c == '*' || c == '+' || c == '?')) {
				i = sk_pair(s, i + 2, n, d, '(', ')', 1);
			} else if (cond && ws && c == '=' && s[i + 1] == '~' && cz_delim(s[i + 2])) {
				i = sk_regex(s, i + 2, n, d);
			} else {
				eq = c == '=';
				i++;
			}
			ws = 0;
		}
		cz_copy(e, s, at, i);
	}
	return n;

bail:
	cz_copy(e, s, at, n);
	return n;
}

size_t compact(char *s, size_t n, size_t code_off, const char *fname)
{
	struct cz_out e = { .o = s, .code_off = code_off, .fname = fname };

	sk_cmd(s, 0, n, 0, &e, 0);
	s[e.keep] = '\0';
	return e.keep;
}

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
