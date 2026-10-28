#include "lang.h"
#include "parser.h"
#include "../util.h"

#include <stdint.h>
#include <string.h>

static int python_claims(str base) {
	return claims_versioned(base, STR("python"));
}

#define PY_DEPTH 32

struct py {
	char*    s;
	size_t   n;
	unsigned depth;
};

static int py_ident(unsigned char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
	       || c == '_' || c >= 0x80;
}

// r, u, b, f and t alone, or r with b, f or t in either order, in any case
static int py_prefix(str w, int* fmt) {
	char a = (char)(w.p[0] | 0x20), b = w.n == 2 ? (char)(w.p[1] | 0x20) : 0;

	if(w.n == 1 && strchr("rubft", a)) {
		*fmt = a == 'f' || a == 't';
		return 1;
	}

	if(w.n != 2 || (a != 'r' && b != 'r')) {
		return 0;
	}

	if(a == 'r') {
		a = b;
	}

	*fmt = a == 'f' || a == 't';

	return a == 'b' || a == 'f' || a == 't';
}

static size_t py_str(const struct py* p, size_t i, int fmt, unsigned d);

// a replacement field of an f-string from after its brace to after the brace that closes it
static size_t py_field(const struct py* p, size_t i, char q, int triple, unsigned d) {
	const char* s = p->s;
	unsigned depth = 0;

	if(d > PY_DEPTH) {
		return SIZE_MAX;
	}

	while(i < p->n) {
		char c = s[i];

		if(c == '\'' || c == '"') {
			i = py_str(p, i, 0, d + 1);
		} else if(py_ident((unsigned char)c) && !(c >= '0' && c <= '9')) {
			size_t e = i;
			int f;

			while(e < p->n && py_ident((unsigned char)s[e])) {
				e++;
			}

			i = (s[e] == '\'' || s[e] == '"') && py_prefix((str){ s + i, e - i }, &f) ? py_str(p, e, f, d + 1) : e;
		} else if(c == '#') {
			while(i < p->n && s[i] != '\n') {
				i++;
			}
		} else if(c == '(' || c == '[' || c == '{') {
			depth++;
			i++;
		} else if((c == ')' || c == ']' || c == '}') && depth) {
			depth--;
			i++;
		} else if(c == '}') {
			return i + 1;
		} else if(c == ':' && !depth) {
			// the format spec is text with fields of its own, the quote of the string ends it
			for(i++; i < p->n && s[i] != '}';) {
				if(s[i] == q || (!triple && s[i] == '\n')) {
					return SIZE_MAX;
				}

				if(s[i] == '{') {
					i = py_field(p, i + 1, q, triple, d + 1);
				} else {
					i += s[i] == '\\' ? 2 : 1;
				}
			}

			return i < p->n ? i + 1 : SIZE_MAX;
		} else if(c == '\r' && s[i + 1] != '\n') {
			return SIZE_MAX;
		} else {
			i++;
		}

		if(i == SIZE_MAX) {
			return SIZE_MAX;
		}
	}

	return SIZE_MAX;
}

// a string from its opening quote to after its closing one, SIZE_MAX when it does not end
static size_t py_str(const struct py* p, size_t i, int fmt, unsigned d) {
	const char* s = p->s;
	char q = s[i];
	int triple = s[i + 1] == q && s[i + 2] == q;

	if(d > PY_DEPTH) {
		return SIZE_MAX;
	}

	if(!triple && s[i + 1] == q) {
		return i + 2;
	}

	i += triple ? 3 : 1;
	while(i < p->n) {
		char c = s[i];

		if(c == '\\') {
			i += s[i + 1] == '\r' && s[i + 2] == '\n' ? 3 : 2;
		} else if(c == q && (!triple || (s[i + 1] == q && s[i + 2] == q))) {
			return i + (triple ? 3 : 1);
		} else if(!triple && (c == '\n' || c == '\r')) {
			return SIZE_MAX;
		} else if(fmt && c == '{') {
			if(s[i + 1] == '{') {
				i += 2;
			} else if((i = py_field(p, i + 1, q, triple, d + 1)) == SIZE_MAX) {
				return SIZE_MAX;
			}
		} else {
			i++;
		}
	}

	return SIZE_MAX;
}

// a lone carriage return ends a line for python and not for the stripper, which then stops
static size_t py_strip(char* s, size_t n, size_t code, const char* fname) {
	struct py p = { .s = s, .n = n };
	size_t i = code, j = code, e;
	char last = n > code ? s[n - 1] : '\n';
	int cont = 0, empty = 1, pend = 0;

	while(i < n) {
		int bol = 1;

		pend = 0;
		for(e = i; e < n && (s[e] == ' ' || s[e] == '\t' || s[e] == '\f'); e++)
			;
		// indentation is syntax only where a logical line begins
		if(!cont && !p.depth) {
			empty = 1;
			if(e < n && s[e] != '\n' && s[e] != '#' && !(s[e] == '\r' && s[e + 1] == '\n')) {
				memmove(s + j, s + i, e - i);
				j += e - i;
			}
		}

		i = e;
		cont = 0;
		while(i < n) {
			char c = s[i];

			if(c == '\n') {
				s[j++] = '\n';
				i++;
				break;
			}

			if(c == ' ' || c == '\t' || c == '\f') {
				pend = 1;
				i++;
				continue;
			}

			if(c == '\r') {
				if(s[i + 1] != '\n') {
					goto bail;
				}

				i++;
				continue;
			}

			if(c == '#') {
				for(e = i; e < n && s[e] != '\n'; e++) {
					if(s[e] == '\r' && s[e + 1] != '\n') {
						goto bail;
					}
				}

				if(bol && s[i + 1] == '%') {
					late_directive(fname, (str){ s + i, e - i });
				}

				pend = 0;
				i = e;
				continue;
			}

			if(pend) {
				s[j++] = ' ';
			}

			pend = bol = 0;
			if(c == '\\') {
				e = s[i + 1] == '\n' ? i + 2 : s[i + 1] == '\r' && s[i + 2] == '\n' ? i + 3
												    : 0;
				if(!e) {
					goto bail;
				}

				s[j++] = '\\';
				s[j++] = '\n';
				i = e;
				// before the first word python takes the indentation of the next line
				cont = !empty;
				break;
			}

			empty = 0;
			e = i + 1;
			if(c == '\'' || c == '"') {
				e = py_str(&p, i, 0, 0);
			} else if(py_ident((unsigned char)c)) {
				int fmt;

				for(e = i; e < n && py_ident((unsigned char)s[e]); e++)
					;
				if((s[e] == '\'' || s[e] == '"') && !(c >= '0' && c <= '9')
				   && py_prefix((str){ s + i, e - i }, &fmt)) {
					e = py_str(&p, e, fmt, 0);
				}
			} else if(c == '(' || c == '[' || c == '{') {
				p.depth++;
			} else if((c == ')' || c == ']' || c == '}') && p.depth) {
				p.depth--;
			}

			if(e == SIZE_MAX) {
				goto bail;
			}

			memmove(s + j, s + i, e - i);
			j += e - i;
			i = e;
		}
	}

	// python counts a last line without a newline, emptied it stays a line
	if(last != '\n' && (j == code || s[j - 1] == '\n')) {
		s[j++] = '\n';
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

static size_t python_store(struct src* s, char* body, size_t len, size_t code_off) {
	size_t n;

	// the header would hide a line python sees after a lone carriage return
	for(size_t k = 0; k < code_off; k++) {
		if(body[k] == '\r' && body[k + 1] != '\n') {
			return len;
		}
	}

	n = blank_header(s, body, len, code_off);

	s->stripped = 1;

	return py_strip(body, n, n - (len - code_off), s->name);
}

static const char* python_check_argv(const struct src* s, char** argv, size_t cap) {
	static char isolated[] = "-I", nocache[] = "-B", dashc[] = "-c";
	// compile() checks the syntax and find_spec() finds the modules the imports at the top name,
	// neither runs any of the script, a dotted name would import its parent packages
	// and any other statement, sys.path.insert() among them, ends the imports looked at
	static char prog[] = "import ast, importlib.util, sys\n"
			     "src = sys.stdin.buffer.read()\n"
			     "try:\n"
			     "    compile(src, 'python', 'exec')\n"
			     "except SyntaxError as e:\n"
			     "    sys.exit('python: line %d: %s' % (e.lineno, e.msg))\n"
			     "for k, node in enumerate(ast.parse(src).body):\n"
			     "    if isinstance(node, ast.Import):\n"
			     "        names = [a.name for a in node.names]\n"
			     "    elif isinstance(node, ast.ImportFrom) and not node.level:\n"
			     "        names = [node.module]\n"
			     "    elif k == 0 and isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant):\n"
			     "        continue\n"
			     "    else:\n"
			     "        break\n"
			     "    for name in names:\n"
			     "        top = name.partition('.')[0]\n"
			     "        if importlib.util.find_spec(top) is None:\n"
			     "            sys.exit(\"python: line %d: no module named '%s'\" % (node.lineno, top))\n";

	argv[0] = (char*)(uintptr_t)s->interp;
	argv[1] = isolated;
	argv[2] = nocache;
	argv[3] = dashc;
	argv[4] = prog;
	argv[5] = NULL;

	return s->interp;
}

// -I keeps / off sys.path, -B writes no __pycache__ as root, -u keeps output in step with the log
static void python_exec_args(struct src* s) {
	strv_push(&s->exec_pre, s->interp);
	strv_push(&s->exec_pre, "-I");
	strv_push(&s->exec_pre, "-B");
	strv_push(&s->exec_pre, "-u");
	for(uint32_t k = 0; k < s->iargs.n; k++) {
		strv_push(&s->exec_pre, s->iargs.v[k]);
	}

	strv_push(&s->exec_pre, "-c");
	strv_push(&s->exec_suf, s->name);
}

// python stops at the first word that is no option and runs it as the script
static const char* python_bad_arg(const struct strv* a, const char** word) {
	for(uint32_t k = 0; k < a->n; k++) {
		const char* w = *word = a->v[k];

		if(w[0] != '-' || !w[1] || !strcmp(w, "--")) {
			return ARG_SCRIPT;
		}

		if(w[1] == '-') {
			if(!strncmp(w, "--help", 6) || !strcmp(w, "--version")) {
				return ARG_EXIT;
			}

			if(!strcmp(w, "--check-hash-based-pycs") && ++k == a->n) {
				return ARG_SCRIPT;
			}

			continue;
		}

		for(const char* c = w + 1; *c; c++) {
			if(*c == 'c' || *c == 'm') {
				return ARG_SCRIPT;
			}

			if(*c == 'h' || *c == '?' || *c == 'V') {
				return ARG_EXIT;
			}

			// the option argument is the rest of the word or the next word
			if(*c == 'W' || *c == 'X') {
				if(!c[1] && ++k == a->n) {
					return ARG_SCRIPT;
				}

				break;
			}
		}
	}

	return NULL;
}

const struct lang lang_python = {
	.name = "python",
	.claims = python_claims,
	.store = python_store,
	.check_argv = python_check_argv,
	.exec_args = python_exec_args,
	.bad_arg = python_bad_arg,
};
