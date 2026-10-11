#include "../../ctl/parser/parser.h"
#include "../../ctl/util.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tap.h"

// the strippers of python, lua and perl against mutated scripts, and against the interpreters

struct lang_case {
	const char *name;
	const struct lang *lang;
	// interpreters tried in turn, the first one installed checks the stripped scripts
	const char *const *interp;
	// argv after the interpreter that prints a canonical form of the file named last
	const char *const *canon;
	const char *const *tokens;
	size_t n_tokens;
	// python counts a last line without a newline, lua and perl count newlines
	int partial;
	// the original is compared with its header blanked, lua cannot read a header
	int blank;
	// how the canonical form of an invalid script begins
	const char *err;
};

// the tokens of the file with their lines, without comments and blank lines, or its syntax error
static const char py_canon_prog[] =
	"import io, sys, tokenize\n"
	"src = open(sys.argv[1], 'rb').read()\n"
	"try:\n"
	"    compile(src, 'f', 'exec')\n"
	"except (SyntaxError, ValueError) as e:\n"
	"    print('error', getattr(e, 'lineno', 0))\n"
	"    sys.exit()\n"
	"skip = {tokenize.COMMENT, tokenize.NL, tokenize.ENCODING}\n"
	"try:\n"
	"    for t in tokenize.tokenize(io.BytesIO(src).readline):\n"
	"        if t.type not in skip:\n"
	"            print(t.start[0], tokenize.tok_name[t.type],\n"
	"                  repr('' if t.type == tokenize.NEWLINE else t.string))\n"
	"except Exception as e:\n"
	"    print('tokenize', type(e).__name__)\n";

static const char *const py_interp[] = { "/usr/bin/python3", "/usr/local/bin/python3", NULL };
static const char *const py_canon[] = { "-I", "-c", py_canon_prog, NULL };
static const char *const py_tokens[] = {
	"#", " # c\n", "'", "\"", "'''", "\"\"\"", "f\"", "f'", "rb'", "Rf\"", "t'", "{", "}", "{{", "}}",
	"(", ")", "[", "]", ":", "!r", "=", "\\", "\\\n", "\n", " ", "\t", "    ", "\r\n", "\r", "x",
	"1", "f\"{x}\"", "f\"{x:>{w}}\"", "#%x: y\n", "if x:\n    pass\n", "\f", "\"\\\"", ";",
};

// the bytecode listing, with the addresses that change from run to run masked, luac skips a first
// line beginning with # in a file, which load() and lua -e do not, so a blank line goes first,
// an error keeps its line, its words quote the text, which differs once comments are gone
static const char lua_canon_prog[] =
	"{ echo; cat \"$1\"; } >\"$1.lua\"; luac -p -l -l \"$1.lua\" 2>&1 | "
	"sed -E 's/0x[0-9a-f]+/ADDR/g; s/^(luac: [^:]*:[0-9]+:).*/\\1/'; rm -f \"$1.lua\"";

static const char *const lua_interp[] = { "/bin/sh", NULL };
static const char *const lua_canon[] = { "-c", lua_canon_prog, "sh", NULL };
static const char *const lua_tokens[] = {
	"--", " -- c\n", "--[[", "]]", "--[==[", "]==]", "[[", "[=[", "]=]", "'", "\"", "\\", "\\z",
	"\n", " ", "\t", "    ", "\r\n", "\r", "\n\r", "x", "1", "-", "=", "(", ")", "{", "}", "[", "]",
	"local x = 1\n", "end\n", "#", "..", "\v", "\f", "\"a\\\nb\"",
};

// the op tree of every sub and the main program, with the line of each statement, in a fixed order
// perl places an error by how far it read, which blanks it skipped change, so of a script it cannot
// compile only that counts, the op tree of one it can carries the line of every statement, and the
// guesses perl prints before it, such as a missing semicolon, look at the text and are left out,
// and so is a reference address folded into a constant, \2 << 3 among them
static const char pl_canon_prog[] =
	"o=$(PERL_HASH_SEED=0 PERL_PERTURB_KEYS=0 perl -X -MO=Concise,-exec,-stash=main \"$1\" 2>&1); r=$?; "
	"case $o in *'syntax OK'*) printf '%s\\n' \"$o\" | sed -E -n '/syntax OK$/,$p' | "
	"sed -E 's/0x[0-9a-f]+/ADDR/g; s/[0-9]{12,}/ADDR/g' ;; "
	"*) echo invalid; exit 1 ;; esac; exit $r";

static const char *const pl_interp[] = { "/bin/sh", NULL };
static const char *const pl_canon[] = { "-c", pl_canon_prog, "sh", NULL };
static const char *const pl_tokens[] = {
	"#", " # c\n", "'", "\"", "`", "/", "//", "s/a/b/", "s{", "}{", "q{", "qw(", "m#", "y/", "tr/",
	"<<\"EOF\"\n", "\nEOF\n", "<<'X'\n", "\nX\n", "<<~E\n", "\n=pod\n", "\n=cut\n", "\n=head1 x\n",
	"$#", "$\"", "${", "@{", "%h", "->", "{", "}", "(", ")", "[", "]", ";", "\n", " ", "\t", "\r\n",
	"$x", "/x/", "=~", "__END__\n", "\n#line 5\n", "-e ", "x", "1", "?", ":", "<", "<<", "=", "=>",
	"\\", "sub f {", "if ($x) {", "else {",
};

static const struct lang_case cases[] = {
	{ "python", &lang_python, py_interp, py_canon, py_tokens, sizeof(py_tokens) / sizeof(*py_tokens), 1, 0, "error" },
	{ "lua", &lang_lua, lua_interp, lua_canon, lua_tokens, sizeof(lua_tokens) / sizeof(*lua_tokens), 0, 1, "luac:" },
	{ "perl", &lang_perl, pl_interp, pl_canon, pl_tokens, sizeof(pl_tokens) / sizeof(*pl_tokens), 0, 1, "" },
};

static char *corpus[64];
static size_t corpus_len[64], n_corpus;

// a file named crlf is kept with lf line ends and read with crlf
static void to_crlf(char **text, size_t *len)
{
	char *out = xmalloc(*len * 2 + 1);
	size_t j = 0;

	for (size_t i = 0; i < *len; i++) {
		if ((*text)[i] == '\n')
			out[j++] = '\r';
		out[j++] = (*text)[i];
	}
	out[j] = '\0';
	free(*text);
	*text = out;
	*len = j;
}

static void load_corpus(const char *lang)
{
	const char *top = getenv("top_srcdir");
	char dir[4096];
	struct dirent **ents;
	int ne;

	for (size_t k = 0; k < n_corpus; k++)
		free(corpus[k]);
	n_corpus = 0;
	snprintf(dir, sizeof(dir), "%s/tests/strip/%s", top ? top : ".", lang);
	ne = scandir(dir, &ents, NULL, alphasort);
	for (int k = 0; k < ne; k++) {
		char path[8192];

		if (ents[k]->d_name[0] != '.' && !strstr(ents[k]->d_name, ".want") && n_corpus < 64) {
			snprintf(path, sizeof(path), "%s/%s", dir, ents[k]->d_name);
			corpus[n_corpus] = slurp(path, &corpus_len[n_corpus]);
			if (!strcmp(ents[k]->d_name, "crlf"))
				to_crlf(&corpus[n_corpus], &corpus_len[n_corpus]);
			n_corpus++;
		}
		free(ents[k]);
	}
	if (ne >= 0)
		free(ents);
}

static size_t mutate(const struct lang_case *c, char *buf, size_t len, size_t cap)
{
	unsigned ops = 1 + tap_below(3);

	for (unsigned o = 0; o < ops; o++) {
		size_t at = len ? tap_below((uint32_t)len + 1) : 0;

		switch (tap_below(4)) {
		case 0: {
			const char *t = c->tokens[tap_below((uint32_t)c->n_tokens)];
			size_t tl = strlen(t);

			if (len + tl >= cap)
				break;
			memmove(buf + at + tl, buf + at, len - at);
			memcpy(buf + at, t, tl);
			len += tl;
			break;
		}
		case 1: {
			size_t dl = 1 + tap_below(8);

			if (at + dl > len)
				dl = len - at;
			memmove(buf + at, buf + at + dl, len - at - dl);
			len -= dl;
			break;
		}
		case 2:
			if (at < len)
				buf[at] = (char)(' ' + tap_below(95));
			break;
		default:
			if (at < len) {
				static const char meta[] = "#'\"`$(){}[]\\\n;|&<>=:-";

				buf[at] = meta[tap_below(sizeof(meta) - 1)];
			}
			break;
		}
	}
	buf[len] = '\0';
	return len;
}

static size_t count_lines(const struct lang_case *c, const char *s, size_t n)
{
	size_t k = c->partial && n && s[n - 1] != '\n';

	for (size_t i = 0; i < n; i++)
		k += s[i] == '\n';
	return k;
}

// the header ends at the first line that is neither blank nor a comment, as in parse_src
static size_t header_end(const char *t, size_t len)
{
	size_t i = 0;

	while (i < len) {
		const char *nl = memchr(t + i, '\n', len - i);
		size_t e = nl ? (size_t)(nl - t) : len, b = i, r = e;

		while (b < e && (t[b] == ' ' || t[b] == '\t'))
			b++;
		while (r > b && (t[r - 1] == ' ' || t[r - 1] == '\t' || t[r - 1] == '\r'))
			r--;
		if (b < r && t[b] != '#')
			return i;
		i = nl ? e + 1 : len;
	}
	return len;
}

// stores TEXT as parse_src would, *code is where the code begins in what it stored
static size_t store(const struct lang_case *c, char *text, size_t len, size_t *code)
{
	static char name[] = "fuzz";
	struct src s = { .name = name };
	size_t h = header_end(text, len);

	*code = 0;
	for (size_t k = 0; k < h; k++)
		*code += text[k] == '\n';
	return c->lang->store(&s, text, len, h);
}

// stores TEXT a second time, its header is the empty lines the first time left
static size_t restore(const struct lang_case *c, char *text, size_t len, size_t code)
{
	static char name[] = "fuzz";
	struct src s = { .name = name };

	return c->lang->store(&s, text, len, code);
}

// TEXT as it is compared, with its header lines empty where the language cannot read them
static size_t original(const struct lang_case *c, char *text, size_t len)
{
	struct src s = { 0 };

	return c->blank ? blank_header(&s, text, len, header_end(text, len)) : len;
}

static void test_structure(const struct lang_case *c)
{
	size_t cap = 1 << 16, bad = 0, iters = 100000;
	char *buf = malloc(cap), *orig = malloc(cap), *again = malloc(cap);

	for (size_t it = 0; it < iters; it++) {
		size_t k = tap_below((uint32_t)n_corpus), len = corpus_len[k], out, out2, nl_in, code;

		if (len >= cap / 2)
			continue;
		memcpy(buf, corpus[k], len);
		len = mutate(c, buf, len, cap / 2);
		memcpy(orig, buf, len + 1);
		nl_in = count_lines(c, buf, len);
		out = store(c, buf, len, &code);
		// a script stored as written keeps its header where it was
		if (out == len && !memcmp(buf, orig, len))
			code = header_end(orig, len);
		memcpy(again, buf, out + 1);
		out2 = restore(c, again, out, code);
		if (out > len || count_lines(c, buf, out) != nl_in || memchr(buf, '\0', out) || buf[out] ||
		    out2 != out || memcmp(again, buf, out)) {
			if (bad++ < 3) {
				tap_diag("iteration %zu: %zu bytes became %zu, then %zu", it, len, out, out2);
				tap_diag_str("input: ", orig, len);
			}
		}
	}
	ok(!bad, "%s: %zu mutated scripts never grow, keep every line and strip to the same text twice",
	   c->name, iters);
	free(buf);
	free(orig);
	free(again);
}

// runs the canonical form of PATH, returns its output and exit status in one allocation
static char *canon(const char *interp, const char *const *argv, const char *path)
{
	const char *av[16];
	size_t n = 0, cap = 1 << 16, len = 0;
	char *out = malloc(cap);
	int p[2], st;
	pid_t pid;

	av[n++] = interp;
	for (; *argv; argv++)
		av[n++] = *argv;
	av[n++] = path;
	av[n] = NULL;
	if (pipe(p) < 0)
		abort();
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		dup2(null, 0);
		dup2(p[1], 1);
		dup2(p[1], 2);
		execv(interp, (char *const *)(uintptr_t)av);
		_exit(127);
	}
	close(p[1]);
	for (;;) {
		ssize_t r;

		if (len + 4096 > cap)
			out = realloc(out, cap *= 2);
		r = read(p[0], out + len, cap - len - 64);
		if (r <= 0)
			break;
		len += (size_t)r;
	}
	close(p[0]);
	waitpid(pid, &st, 0);
	snprintf(out + len, 64, "\nstatus %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	return out;
}

static void put_file(const char *path, const char *text, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0 || write(fd, text, len) != (ssize_t)len)
		abort();
	close(fd);
}

static void test_interp(const struct lang_case *c)
{
	const char *env = getenv("NINIT_TEST_FUZZ");
	size_t iters = env ? strtoul(env, NULL, 10) : 200, cap = 1 << 16, bad = 0, same_err = 0;
	char *buf = malloc(cap), *orig = malloc(cap), *cmp = malloc(cap);
	char dir[] = "/tmp/ninit-t-strip.XXXXXX", pa[64];
	const char *interp = NULL;

	for (const char *const *p = c->interp; *p; p++)
		if (!access(*p, X_OK)) {
			interp = *p;
			break;
		}
	// the lua listing comes from luac in the PATH, the perl one from perl
	if ((c->lang == &lang_lua && system("command -v luac >/dev/null 2>&1")) ||
	    (c->lang == &lang_perl && system("command -v perl >/dev/null 2>&1")))
		interp = NULL;
	if (!interp) {
		ok(1, "# SKIP %s: no interpreter installed", c->name);
		free(buf);
		free(orig);
		free(cmp);
		return;
	}
	if (!mkdtemp(dir))
		abort();
	// one name for both, interpreters print it in messages
	snprintf(pa, sizeof(pa), "%s/a", dir);
	for (size_t it = 0; it < iters; it++) {
		size_t k = tap_below((uint32_t)n_corpus), len = corpus_len[k], out, code, cl;
		char *ca, *cb;

		memcpy(buf, corpus[k], len);
		len = mutate(c, buf, len, cap / 2);
		memcpy(orig, buf, len + 1);
		out = store(c, buf, len, &code);
		memcpy(cmp, orig, len + 1);
		cl = original(c, cmp, len);
		// nothing changed, nothing to compare, perl would fold a reference address differently
		if (cl == out && !memcmp(cmp, buf, out))
			continue;
		put_file(pa, cmp, cl);
		ca = canon(interp, c->canon, pa);
		put_file(pa, buf, out);
		cb = canon(interp, c->canon, pa);
		same_err += *c->err ? !strncmp(ca, c->err, strlen(c->err)) : !strstr(ca, "syntax OK");
		if (strcmp(ca, cb) && bad++ < 3) {
			tap_diag("%s: the stripped script reads differently", c->name);
			tap_diag_str("input:    ", orig, len);
			tap_diag_str("stripped: ", buf, out);
			tap_diag_str("original: ", ca, strlen(ca));
			tap_diag_str("now:      ", cb, strlen(cb));
		}
		free(ca);
		free(cb);
	}
	ok(!bad, "%s: %zu mutated scripts read the same to %s once stripped, %zu of them invalid",
	   c->name, iters, interp, same_err);
	unlink(pa);
	rmdir(dir);
	free(buf);
	free(orig);
	free(cmp);
}

int main(void)
{
	const char *seed = getenv("NINIT_TEST_SEED");

	if (seed)
		tap_rng_state ^= strtoull(seed, NULL, 0) * 0x2545f4914f6cdd1dull;
	g_dir = ".";
	// the warning about a late #% line goes nowhere
	if (!freopen("/dev/null", "w", stderr))
		abort();
	for (size_t k = 0; k < sizeof(cases) / sizeof(*cases); k++) {
		load_corpus(cases[k].name);
		if (!ok(n_corpus >= 3, "%s: the corpus is present (%zu files)", cases[k].name, n_corpus))
			continue;
		test_structure(&cases[k]);
		test_interp(&cases[k]);
	}
	return tap_done();
}
