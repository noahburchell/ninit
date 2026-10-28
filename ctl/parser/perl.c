#include "lang.h"
#include "parser.h"
#include "../util.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

static int perl_claims(str base) {
	return claims_versioned(base, STR("perl"));
}

#define PL_TAG	128
#define PL_HD	8
#define PL_NEST 64

// what perl expects after the previous token, which decides what / and << are
enum pl_expect {
	PL_STMT,  // a statement, after ; and where the code begins
	PL_TERM,  // a term, after an operator
	PL_OPER,  // an operator, after a term
	PL_OPEN,  // a term or a statement, after a { that does not end a term
	PL_BRACE, // a term or an operator, after a } that perl alone can tell
	PL_WORD,  // a term or an operator, after a bareword perl alone can tell
};

// what a { opened, its } leaves perl expecting a statement, an operator or either
enum {
	PL_BLOCK,
	PL_SUBSCRIPT,
	PL_EITHER
};

struct pl_hd {
	char   tag[PL_TAG];
	size_t len;
	int    indent;
};

static int pl_word(unsigned char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
	       || c == '_' || c >= 0x80;
}

static int pl_alpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int pl_blank(char c) {
	return c == ' ' || c == '\t' || c == '\f';
}

// a word with its :: parts, from i
static size_t pl_ident(const char* s, size_t i) {
	for(;;) {
		while(pl_word((unsigned char)s[i])) {
			i++;
		}

		if(s[i] != ':' || s[i + 1] != ':') {
			return i;
		}

		i += 2;
	}
}

static int pl_is(str w, const char* list) {
	for(const char* p = list; *p;) {
		size_t l = strlen(p);

		if(str_eq(w, (str){ p, l })) {
			return 1;
		}

		p += l + 1;
	}

	return 0;
}

// after one delimited part that opens at s[i], nesting brackets, SIZE_MAX when it does not end
static size_t pl_part(const char* s, size_t i, size_t n) {
	char o = s[i], c = o == '(' ? ')' : o == '[' ? ']'
				    : o == '{'	     ? '}'
				    : o == '<'	     ? '>'
						     : o;
	unsigned depth = 1;

	for(i++; i < n; i++) {
		if(s[i] == '\\') {
			i++;
		} else if(c != o && s[i] == o) {
			depth++;
		} else if(s[i] == c && !--depth) {
			return i + 1;
		}
	}

	return SIZE_MAX;
}

// a heredoc inside quoted text, such as s///e, has a body the stripper would not see
static int pl_hidden_hd(const char* s, size_t i, size_t e) {
	for(; i + 2 < e; i++) {
		if(s[i] == '<' && s[i + 1] == '<' && (strchr("~\"'\\_", s[i + 2]) || pl_alpha(s[i + 2]))) {
			return 1;
		}
	}

	return 0;
}

// after a quote-like operator whose delimiter is at s[i], PARTS is 2 for s, tr and y
static size_t pl_quote(const char* s, size_t i, size_t n, int parts, int mods) {
	size_t e = pl_part(s, i, n);

	if(e != SIZE_MAX && parts == 2) {
		if(strchr("([{<", s[i])) {
			while(s[e] == ' ' || s[e] == '\t') {
				e++;
			}

			e = !s[e] || pl_word((unsigned char)s[e]) || strchr(" \t\r\n#=,;", s[e]) ? SIZE_MAX : pl_part(s, e, n);
		} else {
			e = pl_part(s, e - 1, n);
		}
	}

	if(e != SIZE_MAX && mods) {
		while(pl_alpha(s[e])) {
			e++;
		}
	}

	return e;
}

// the tag of the heredoc at s[i], after <<, SIZE_MAX for a form the stripper does not follow
static size_t pl_hd_tag(const char* s, size_t i, struct pl_hd* h) {
	size_t k = i + 2, b;

	h->indent = s[k] == '~';
	k += (size_t)h->indent;
	if(s[k] == ' ' || s[k] == '\t') {
		while(s[k] == ' ' || s[k] == '\t') {
			k++;
		}

		if(s[k] != '"' && s[k] != '\'') {
			return SIZE_MAX;
		}
	}

	if(s[k] == '"' || s[k] == '\'' || s[k] == '`') {
		char q = s[k++];

		for(b = k; s[k] != q; k++) {
			if(!s[k] || s[k] == '\\' || s[k] == '\n') {
				return SIZE_MAX;
			}
		}

		h->len = k++ - b;
	} else {
		k += s[k] == '\\';
		if(!pl_word((unsigned char)s[k]) || (s[k] >= '0' && s[k] <= '9')) {
			return SIZE_MAX;
		}

		for(b = k; pl_word((unsigned char)s[k]); k++)
			;
		h->len = k - b;
	}

	if(h->len >= PL_TAG) {
		return SIZE_MAX;
	}

	memcpy(h->tag, s + b, h->len);

	return k;
}

// after the body of a heredoc that begins at i, its last line the tag alone, perl takes crlf for lf
static size_t pl_hd_body(const char* s, size_t i, size_t n, const struct pl_hd* h) {
	while(i < n) {
		const char* nl = memchr(s + i, '\n', n - i);
		size_t eol = nl ? (size_t)(nl - s) : n, b = i, le = eol;

		if(h->indent) {
			while(b < eol && (s[b] == ' ' || s[b] == '\t')) {
				b++;
			}
		}

		if(nl && le > b && s[le - 1] == '\r') {
			le--;
		}

		if(le - b == h->len && !memcmp(s + b, h->tag, h->len)) {
			return nl ? eol + 1 : n;
		}

		i = nl ? eol + 1 : n;
	}

	return SIZE_MAX;
}

// after the pod that begins at i, its =cut line included, pod with no =cut runs to the end
static size_t pl_pod(const char* s, size_t i, size_t n) {
	while(i < n) {
		const char* nl = memchr(s + i, '\n', n - i);

		i = nl ? (size_t)(nl - s) + 1 : n;
		if(i < n && !memcmp(s + i, "=cut", 4) && !pl_word((unsigned char)s[i + 4])) {
			nl = memchr(s + i, '\n', n - i);
			return nl ? (size_t)(nl - s) : n;
		}
	}

	return n;
}

static int pl_digit(char c, int hex) {
	return (c >= '0' && c <= '9') || c == '_' || (hex && ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'));
}

// after the number at s[i], 0x1f, 0b101, 1_000, .5 and 1.5e-3 among them
static size_t pl_number(const char* s, size_t i) {
	if(s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'b' || s[i + 1] == 'B')) {
		for(i += 2; pl_digit(s[i], 1); i++)
			;
		return i;
	}

	while(pl_digit(s[i], 0)) {
		i++;
	}

	if(s[i] == '.' && s[i + 1] >= '0' && s[i + 1] <= '9') {
		for(i++; pl_digit(s[i], 0); i++)
			;
	}

	if((s[i] == 'e' || s[i] == 'E') && (pl_digit(s[i + 1], 0) || ((s[i + 1] == '-' || s[i + 1] == '+') && pl_digit(s[i + 2], 0)))) {
		for(i += 2; pl_digit(s[i], 0); i++)
			;
	}

	return i;
}

// #line N "file" renumbers the lines in messages and stays
static int pl_line_directive(const char* s) {
	s++;
	while(*s == ' ' || *s == '\t') {
		s++;
	}

	return !strncmp(s, "line", 4) && (s[4] == ' ' || s[4] == '\t');
}

// a construct whose meaning depends on more than the token before it stops the stripper
static size_t pl_strip(char* s, size_t n, size_t code, const char* fname) {
	static const char opwords[] = "and\0or\0not\0xor\0if\0unless\0while\0until\0lt\0gt\0le\0ge\0eq\0ne\0"
				      "cmp\0isa\0";
	static const char termwords[] = "return\0split\0grep\0map\0join\0push\0unshift\0print\0printf\0say\0"
					"die\0warn\0eval\0do\0defined\0ref\0scalar\0my\0our\0local\0state\0"
					"sort\0reverse\0keys\0values\0each\0delete\0exists\0chomp\0chop\0lc\0uc\0"
					"length\0sprintf\0open\0close\0unlink\0exit\0foreach\0for\0elsif\0when\0";
	static const char blockwords[] = "else\0BEGIN\0END\0INIT\0CHECK\0UNITCHECK\0default\0";
	struct pl_hd hd[PL_HD];
	uint8_t nest[PL_NEST];
	unsigned nhd = 0, depth = 0, parens = 0;
	size_t i = code, j = code, e;
	enum pl_expect ex = PL_STMT;
	int pend = 0, arrow = 0, block = 0, sub = 0, fhword = 0, fh = 0;

	while(i < n) {
		size_t ls = i;
		int bol = 1;

		pend = 0;
		while(i < n && pl_blank(s[i])) {
			i++;
		}

		// =word at the start of a line can begin pod, an indented one keeps a blank
		pend = i > ls && s[i] == '=' && pl_alpha(s[i + 1]);
		while(i < n) {
			char c = s[i], last, before;
			int bl = block, sb = sub, fw = fhword;

			if(c == '\n') {
				s[j++] = '\n';
				i++;
				pend = 0;
				for(unsigned k = 0; k < nhd; k++) {
					e = pl_hd_body(s, i, n, &hd[k]);
					if(e == SIZE_MAX) {
						goto bail;
					}

					memmove(s + j, s + i, e - i);
					j += e - i;
					i = e;
				}

				nhd = 0;
				break;
			}

			if(pl_blank(c)) {
				pend = !bol;
				i++;
				continue;
			}

			// a carriage return alone moves the lines of a heredoc in perl, the rest stays as written
			if(c == '\r') {
				if(s[i + 1] != '\n') {
					goto bail;
				}

				i++;
				continue;
			}

			if(c == '#') {
				for(e = i; e < n && s[e] != '\n'; e++)
					;
				if(i == ls && pl_line_directive(s + i)) {
					memmove(s + j, s + i, e - i);
					j += e - i;
				} else if(bol && s[i + 1] == '%') {
					late_directive(fname, (str){ s + i, e - i });
				}

				pend = 0;
				i = e;
				continue;
			}

			// pod begins where perl expects a statement, its lines stay empty
			if(c == '=' && i == ls && pl_alpha(s[i + 1])) {
				if(ex == PL_STMT && !parens && memcmp(s + i, "=cut", 4)) {
					e = pl_pod(s, i, n);
					// perl reads #line in pod too
					for(size_t k = i; k < e; k++) {
						if(s[k] != '\n') {
							continue;
						}

						s[j++] = '\n';
						if(k + 1 < e && s[k + 1] == '#' && pl_line_directive(s + k + 1)) {
							while(k + 1 < e && s[k + 1] != '\n') {
								s[j++] = s[++k];
							}
						}
					}

					i = e;
					continue;
				}

				if(ex != PL_TERM && ex != PL_OPER) {
					goto bail;
				}
			}

			if(pend) {
				s[j++] = ' ';
			}

			pend = bol = 0;
			block = sub = fhword = 0;
			last = j > code ? s[j - 1] : '\n';
			before = j > code + 1 ? s[j - 2] : '\n';
			e = i + 1;

			// print $fh <<EOF, a variable after print or a bareword may be a filehandle
			if(ex == PL_OPER && fh && (c == '/' || (c == '<' && s[e] == '<'))) {
				goto bail;
			}

			fh = 0;
			if(c == '$') {
				fh = fw || ex == PL_WORD;
				if(s[e] == '#' && (s[e + 1] == '{' || s[e + 1] == '$')) {
					e++;
					ex = PL_TERM;
					goto copy;
				}

				e += s[e] == '#';
				while(s[e] == '$' && (pl_word((unsigned char)s[e + 1]) || (s[e + 1] && strchr("${:", s[e + 1])))) {
					e++;
				}

				if(s[e] == '{') {
					ex = PL_TERM;
					goto copy;
				}

				if(s[e] == '^' && s[e + 1] >= 'A' && s[e + 1] <= 'Z') {
					e += 2;
				} else if(pl_word((unsigned char)s[e]) || (s[e] == ':' && s[e + 1] == ':')) {
					e = pl_ident(s, e);
				} else if(e == i + 1 && s[e] && !pl_blank(s[e]) && s[e] != '\n') {
					e++;
				} else if(e == i + 1) {
					goto bail;
				}

				ex = PL_OPER;
			} else if(c == '@' || ((c == '%' || c == '&' || c == '*') && ex != PL_OPER)) {
				if(s[e] == '{' || s[e] == '$') {
					ex = PL_TERM;
				} else if(pl_word((unsigned char)s[e]) || (s[e] == ':' && s[e + 1] == ':')) {
					e = pl_ident(s, e);
					ex = PL_OPER;
				} else if((c == '@' || c == '%') && (s[e] == '-' || s[e] == '+')) {
					e++;
					ex = PL_OPER;
				} else {
					goto bail;
				}
			} else if(c == '\'' || c == '"' || c == '`') {
				if(c == '\'' && i > code && pl_word((unsigned char)s[i - 1])) {
					goto bail;
				}

				e = pl_part(s, i, n);
				if(e == SIZE_MAX || pl_hidden_hd(s, i, e)) {
					goto bail;
				}

				ex = PL_OPER;
			} else if(c == '/' && ex != PL_OPER) {
				if(ex != PL_STMT && ex != PL_TERM && ex != PL_OPEN) {
					goto bail;
				}

				e = pl_quote(s, i, n, 1, 1);
				if(e == SIZE_MAX || pl_hidden_hd(s, i, e)) {
					goto bail;
				}

				ex = PL_OPER;
			} else if(c == '<' && s[e] == '<' && ex != PL_OPER
				  && ((s[e + 1] && strchr("~\"'\\_ \t", s[e + 1])) || pl_alpha(s[e + 1]))) {
				if(ex != PL_STMT && ex != PL_TERM && ex != PL_OPEN) {
					goto bail;
				}

				if(nhd == PL_HD || (e = pl_hd_tag(s, i, &hd[nhd])) == SIZE_MAX) {
					goto bail;
				}

				nhd++;
				ex = PL_OPER;
			} else if(c == '<' && ex != PL_OPER && s[e] != '<' && s[e] != '=') {
				// <FH>, <$fh> and <*.c> read or glob
				while(e < n && !strchr(">\n#\"'", s[e])) {
					e++;
				}

				if(s[e] != '>') {
					goto bail;
				}

				e++;
				ex = PL_OPER;
			} else if((c >= '0' && c <= '9') || (c == '.' && ex != PL_OPER && s[e] >= '0' && s[e] <= '9')) {
				e = pl_number(s, i);
				// a word right after a number, as in 4m#2#, is lexed as perl alone knows
				if(pl_word((unsigned char)s[e])) {
					goto bail;
				}

				ex = PL_OPER;
			} else if(pl_word((unsigned char)c)) {
				int parts = 0, mods = 0;
				size_t k;
				str w;

				e = pl_ident(s, i);
				w = (str){ s + i, e - i };
				if((s[e] == '\'' && pl_word((unsigned char)s[e + 1])) || pl_is(w, "__END__\0__DATA__\0")) {
					goto bail;
				}

				if(arrow) {
					arrow = 0;
					ex = PL_OPER;
					goto copy;
				}

				for(k = e; s[k] == ' ' || s[k] == '\t'; k++)
					;
				if(s[k] == '=' && s[k + 1] == '>') {
					ex = PL_OPER;
					goto copy;
				}

				if(pl_is(w, "q\0qq\0qw\0qx\0")) {
					parts = 1;
				} else if(pl_is(w, "m\0qr\0")) {
					parts = mods = 1;
				} else if(pl_is(w, "s\0tr\0y\0")) {
					parts = 2;
					mods = 1;
				}

				if(parts) {
					// after blanks # starts a comment, and a word is no delimiter
					if((k > e && !strchr("{([</|!\"'", s[k])) || !s[k]
					   || pl_word((unsigned char)s[k]) || strchr(" \t\r\n=,;)}", s[k])
					   || (i > code && s[i - 1] == '-')) {
						goto bail;
					}

					e = pl_quote(s, k, n, parts, mods);
					if(e == SIZE_MAX || pl_hidden_hd(s, i, e)) {
						goto bail;
					}

					ex = PL_OPER;
				} else if(ex == PL_STMT && pl_is(w, "format\0")) {
					goto bail;
				} else if(pl_is(w, "__FILE__\0__LINE__\0__PACKAGE__\0__SUB__\0")) {
					ex = PL_OPER;
				} else if(pl_is(w, opwords) || pl_is(w, termwords)
					  || (str_eq(w, STR("x")) && ex == PL_OPER)) {
					fhword = pl_is(w, "print\0printf\0say\0exec\0system\0sort\0");
					ex = PL_TERM;
				} else {
					// the block after else, BEGIN or the name of a sub ends a statement
					block = pl_is(w, blockwords) || sb;
					sub = str_eq(w, STR("sub"));
					ex = PL_WORD;
				}
			} else if(c == '-' && s[e] == '>') {
				e++;
				// a method name follows, or a subscript or call
				arrow = pl_word((unsigned char)s[e]);
				ex = PL_TERM;
			} else if(c == '-' && ex != PL_OPER && s[e] && strchr("rwxoRWXOezsfdlpSbctugkTBAMC", s[e])
				  && !pl_word((unsigned char)s[e + 1])) {
				// a file test such as -e or -s
				e++;
				ex = PL_TERM;
			} else if((c == '-' || c == '+') && s[e] == c) {
				// $i++ is still a term, ++$i wants one
				e++;
				ex = ex == PL_OPER ? PL_OPER : PL_TERM;
			} else if(c == '{') {
				if(depth == PL_NEST) {
					goto bail;
				}

				// right after a variable, ] or } it is a subscript, after ) or else it is a block
				if((ex == PL_OPER || ex == PL_BRACE)
				   && (pl_word((unsigned char)last) || last == ']' || last == '}')) {
					nest[depth++] = PL_SUBSCRIPT;
				} else if(ex == PL_TERM && last == '>' && before == '-') {
					nest[depth++] = PL_SUBSCRIPT;
				} else if(bl || last == ')' || (last == ' ' && before == ')')) {
					nest[depth++] = PL_BLOCK;
				} else {
					nest[depth++] = PL_EITHER;
				}

				ex = PL_OPEN;
			} else if(c == '}') {
				if(!depth) {
					goto bail;
				}

				depth--;
				ex = nest[depth] == PL_BLOCK ? PL_STMT : nest[depth] == PL_SUBSCRIPT ? PL_OPER
												     : PL_BRACE;
			} else if(c == '(' || c == '[') {
				parens += c == '(';
				ex = PL_TERM;
			} else if(c == ')' || c == ']') {
				parens -= c == ')' && parens;
				ex = PL_OPER;
			} else if(c == ';') {
				ex = PL_STMT;
			} else if(c == '\\' && (s[e] == '\n' || !s[e])) {
				goto bail;
			} else {
				// an operator, two of / & * < > | . make one, and = may follow
				if(strchr("/&*<>|.", c) && s[e] == c) {
					e++;
				} else if((c == '=' && s[e] && strchr("~=>", s[e])) || (c == '!' && s[e] == '~')) {
					e++;
				}

				ex = PL_TERM;
			}
copy:
			memmove(s + j, s + i, e - i);
			j += e - i;
			i = e;
		}
	}

	if(nhd) {
		goto bail;
	}

	s[j] = '\0';
	return j;

bail:
	if(pend) {
		s[j++] = ' ';
	}

	memmove(s + j, s + i, n - i + 1);

	return j + n - i;
}

static size_t perl_store(struct src* s, char* body, size_t len, size_t code_off) {
	size_t n = blank_header(s, body, len, code_off);

	s->stripped = 1;

	return pl_strip(body, n, n - (len - code_off), s->name);
}

// perl -c runs BEGIN blocks and use statements, there is no check that runs nothing
static const char* perl_check_argv(const struct src* s, char** argv, size_t cap) {
	static char check[] = "-c", stdin_prog[] = "-";

	argv[0] = (char*)(uintptr_t)s->interp;
	argv[1] = check;
	argv[2] = stdin_prog;
	argv[3] = NULL;

	return s->interp;
}

// a first line flushes stdout per line for the log and names the script, #line restores line 1
static void perl_exec_args(struct src* s) {
	char* pre = escape_name(STR("$| = 1; $0 = '"), s->name, '\'', STR("';"));

	strv_push(&s->exec_pre, s->interp);
	for(uint32_t k = 0; k < s->iargs.n; k++) {
		strv_push(&s->exec_pre, s->iargs.v[k]);
	}

	strv_push(&s->exec_pre, "-e");
	strv_push(&s->exec_pre, pre);
	strv_push(&s->exec_pre, "-e");
	strv_push(&s->exec_pre, "#line 1");
	strv_push(&s->exec_pre, "-e");
}

// the reason one switch cluster is refused, *next is set when its argument is the next word
static const char* perl_cluster(const char* c, int* next) {
	for(; *c; c++) {
		switch(*c) {
			case 'e':
			case 'E':
				return ARG_CODE;
			case 'x':
				return ARG_SCRIPT;
			case 'c':
			case 'h':
			case 'u':
			case 'v':
			case 'V':
				return ARG_EXIT;
			case 'a':
			case 'F':
			case 'n':
			case 'p':
				return ARG_LOOP;
			case 'I':
				*next = !c[1];
				return NULL;
			// without a module -d is the interactive debugger, reading /dev/null
			case 'd':
				c += c[1] == 't';
				return c[1] == ':' ? NULL : ARG_EXIT;
			case 'D':
			case 'i':
			case 'm':
			case 'M':
				return NULL;
			case '0':
				if(c[1] == 'x' || c[1] == 'X') {
					for(c++; isxdigit((unsigned char)c[1]); c++)
						;
				} else {
					for(; c[1] >= '0' && c[1] <= '7'; c++)
						;
				}

				break;
			case 'l':
				for(; c[1] >= '0' && c[1] <= '7'; c++)
					;
				break;
			case 'C':
				for(; c[1] && strchr("0123456789IOEioSALaD", c[1]); c++)
					;
				break;
			default:
				break;
		}
	}

	return NULL;
}

// perl takes the first word that is no switch as the script, -x discards a script without #!perl
static const char* perl_bad_arg(const struct strv* a, const char** word) {
	for(uint32_t k = 0; k < a->n; k++) {
		const char* w = *word = a->v[k];
		const char* why;
		int next = 0;

		if(w[0] != '-' || !w[1] || !strcmp(w, "--")) {
			return ARG_SCRIPT;
		}

		why = perl_cluster(w + 1, &next);
		if(why) {
			return why;
		}

		if(next && ++k == a->n) {
			return ARG_SCRIPT;
		}
	}

	return NULL;
}

const struct lang lang_perl = {
	.name = "perl",
	.claims = perl_claims,
	.store = perl_store,
	.check_argv = perl_check_argv,
	.exec_args = perl_exec_args,
	.bad_arg = perl_bad_arg,
};
