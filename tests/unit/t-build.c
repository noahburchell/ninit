#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tap.h"

static jmp_buf die_jb;

[[noreturn]] static void shim_exit(int code)
{
	longjmp(die_jb, 1000 + code);
}

#define exit shim_exit
#include "../../ctl/util.c"
#include "../../ctl/parser/compact.c"
#include "../../ctl/parser/directives.c"
#include "../../ctl/parser/lang.c"
#include "../../ctl/parser/shell.c"
#include "../../ctl/parser/python.c"
#include "../../ctl/parser/lua.c"
#include "../../ctl/parser/perl.c"
#include "../../ctl/output.c"
#include "../../ctl/sources.c"
#include "../../ctl/graph.c"
#include "../../ctl/check.c"
#include "../../ctl/image.c"
#include "../../ctl/build.c"
#undef exit

static char errtext[4096];

// runs fn with stderr captured, returns 0 or 1000 + the status die() exited with
static int capture(void (*fn)(void *), void *arg)
{
	char path[] = "/tmp/ninit-t-build-err.XXXXXX";
	int fd = mkstemp(path), saved;
	volatile int rc;
	ssize_t n;

	fflush(stderr);
	saved = dup(2);
	dup2(fd, 2);
	rc = setjmp(die_jb);
	if (!rc)
		fn(arg);
	fflush(stderr);
	dup2(saved, 2);
	close(saved);
	lseek(fd, 0, SEEK_SET);
	n = read(fd, errtext, sizeof(errtext) - 1);
	errtext[n > 0 ? n : 0] = '\0';
	close(fd);
	unlink(path);
	return rc;
}

struct ms_arg {
	const char *val;
	uint32_t got;
};

static void call_ms(void *p)
{
	struct ms_arg *a = p;

	a->got = parse_ms(a->val, "svc", "start-timeout");
}

static void test_parse_ms(void)
{
	static const struct {
		const char *val;
		uint32_t ms;
	} good[] = {
		{ "1", 1 }, { "1ms", 1 }, { "250ms", 250 }, { "1s", 1000 }, { "90s", 90000 },
		{ "2m", 120000 }, { "1h", 3600000 }, { "24h", 86400000 }, { "1440m", 86400000 },
		{ "86400s", 86400000 }, { "86400000", 86400000 }, { "007s", 7000 },
	};
	static const struct {
		const char *val, *why;
	} bad[] = {
		{ "0", "start-timeout:0 must not be zero" },
		{ "0s", "start-timeout:0s must not be zero" },
		{ "0h", "start-timeout:0h must not be zero" },
		{ "", "start-timeout: is not a duration" },
		{ "-1", "start-timeout:-1 is not a duration" },
		{ "-1s", "start-timeout:-1s is not a duration" },
		{ "s", "start-timeout:s is not a duration" },
		{ "ten", "start-timeout:ten is not a duration" },
		{ "1x", "start-timeout:1x has an unknown unit 'x', expected ms, s, m or h" },
		{ "1S", "start-timeout:1S has an unknown unit 'S', expected ms, s, m or h" },
		{ "1sec", "start-timeout:1sec has an unknown unit 'sec', expected ms, s, m or h" },
		{ "1 s", "start-timeout:1 s has an unknown unit ' s', expected ms, s, m or h" },
		{ "1.5s", "start-timeout:1.5s has an unknown unit '.5s', expected ms, s, m or h" },
		{ "0x10", "start-timeout:0x10 has an unknown unit 'x10', expected ms, s, m or h" },
		{ "86400001", "start-timeout:86400001 exceeds the 86400000 ms maximum" },
		{ "86401s", "start-timeout:86401s exceeds the 86400000 ms maximum" },
		{ "25h", "start-timeout:25h exceeds the 86400000 ms maximum" },
		{ "1441m", "start-timeout:1441m exceeds the 86400000 ms maximum" },
		{ "99999999999999999999", "start-timeout:99999999999999999999 is not a duration" },
		{ "18446744073709551615h", "start-timeout:18446744073709551615h exceeds the 86400000 ms maximum" },
	};

	g_dir = "/d";
	for (size_t k = 0; k < sizeof(good) / sizeof(*good); k++) {
		struct ms_arg a = { good[k].val, 0 };
		int rc = capture(call_ms, &a);

		ok(!rc && a.got == good[k].ms, "duration %s is %u ms", good[k].val, good[k].ms);
		if (rc || a.got != good[k].ms)
			tap_diag("rc %d got %u: %s", rc, a.got, errtext);
	}
	for (size_t k = 0; k < sizeof(bad) / sizeof(*bad); k++) {
		struct ms_arg a = { bad[k].val, 0 };
		char want[256];
		int rc = capture(call_ms, &a);

		snprintf(want, sizeof(want), "ninitctl: /d/svc: %s\n", bad[k].why);
		ok(rc == 1001 && !strcmp(errtext, want), "duration '%s' is refused", bad[k].val);
		if (rc != 1001 || strcmp(errtext, want)) {
			tap_diag("rc %d", rc);
			tap_diag_str("got:  ", errtext, strlen(errtext));
			tap_diag_str("want: ", want, strlen(want));
		}
	}
}

static void test_helpers(void)
{
	{
		char v[] = " a, b,c  d\t,e,, ,f ";
		struct strv s = { 0 };

		split_into(&s, v, 1);
		ok(s.n == 6 && !strcmp(s.v[0], "a") && !strcmp(s.v[1], "b") && !strcmp(s.v[2], "c") &&
		   !strcmp(s.v[3], "d") && !strcmp(s.v[4], "e") && !strcmp(s.v[5], "f"),
		   "names split on commas, blanks and both");
		free(s.v);
	}
	{
		char v[] = ", ,,\t";
		struct strv s = { 0 };

		split_into(&s, v, 1);
		is_int(s.n, 0, "separators alone give no names");
		free(s.v);
	}
	{
		char v[] = "a,b c";
		struct strv s = { 0 };

		split_into(&s, v, 0);
		ok(s.n == 2 && !strcmp(s.v[0], "a,b"), "without the comma flag only blanks separate");
		free(s.v);
	}
	{
		char v[] = " \t a b \t\r";

		is_str(trim(v), "a b", "trim removes blanks around a value and a final cr");
	}
	is_int(has_code(""), 0, "an empty file has no commands");
	is_int(has_code("#!/bin/bash\n# c\n\n \t\n#%type: target\n"), 0, "comments and blanks are not commands");
	is_int(has_code("\r\n\r\n"), 0, "cr line ends are blank");
	is_int(has_code("# c\n  :\n"), 1, "an indented command counts");
	is_int(has_code("x"), 1, "a command without a newline counts");
	is_int(cz_lead("time -p x", CZ_KW | CZ_TIME) & CZ_KW, CZ_KW, "time is a reserved word");
	is_int(cz_lead("-p x", CZ_TIME), CZ_KW | CZ_TIME, "-p after time keeps reserved words possible");
	is_int(cz_lead("for x", CZ_KW), CZ_NAME, "for is followed by a name");
	is_int(cz_lead("fi;", CZ_KW), CZ_KW, "a reserved word may end at a metacharacter");
	is_int(cz_lead("fix", CZ_KW), 0, "a reserved word must end at a delimiter");
	ok(is_artifact("depgraph", "depgraph", 8), "the output itself is an artifact");
	ok(is_artifact("depgraph.old", "depgraph", 8), "its .old is an artifact");
	ok(is_artifact("depgraph.tmp", "depgraph", 8), "its .tmp is an artifact");
	ok(!is_artifact("depgraph.bak", "depgraph", 8), "other suffixes are not artifacts");
	ok(!is_artifact("x", NULL, 0), "without an output in the directory nothing is an artifact");
}

static char *dup_n(const char *s, size_t n)
{
	char *p = malloc(n + 1);

	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

static size_t count_nl(const char *s, size_t n)
{
	size_t c = 0;

	for (size_t k = 0; k < n; k++)
		c += s[k] == '\n';
	return c;
}

static size_t do_compact(char *s, size_t n)
{
	return compact(s, n, n, "fuzz");
}

static char **corpus;
static size_t *corpus_len, n_corpus;

static void load_corpus(void)
{
	const char *top = getenv("top_srcdir");
	char dir[4096];
	struct dirent **ents;
	int ne;

	if (!top)
		top = ".";
	snprintf(dir, sizeof(dir), "%s/tests/compact", top);
	ne = scandir(dir, &ents, NULL, alphasort);
	if (ne < 0)
		return;
	corpus = calloc((size_t)ne, sizeof(*corpus));
	corpus_len = calloc((size_t)ne, sizeof(*corpus_len));
	for (int k = 0; k < ne; k++) {
		char path[8192];
		size_t len;

		if (ents[k]->d_name[0] == '.' || strstr(ents[k]->d_name, ".want")) {
			free(ents[k]);
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, ents[k]->d_name);
		corpus[n_corpus] = slurp(path, &len);
		corpus_len[n_corpus++] = len;
		free(ents[k]);
	}
	free(ents);
}

static const char *const tokens[] = {
	"#", " # c\n", "<<EOF\n", "\nEOF\n", "<<-X\n\tX\n", "<<'Q'\nq\nQ\n", "\\\n", "\\", "$(", ")", "(",
	"'", "\"", "`", "${", "}", "{", "case x in", " in ", "esac", ";;", ";&", "[[", "]]", "=~",
	"((", "))", "$((", "\n", " ", "\t", "a=(", "[", "]", "=", "@(", "!(", "time", "coproc",
	"for", "do", "done", "<(", ">(", "<<<", "&", "|", ";", "$'", "#%x: y\n", "\r",
};

static size_t mutate(char *buf, size_t len, size_t cap)
{
	unsigned ops = 1 + tap_below(3);

	for (unsigned o = 0; o < ops; o++) {
		size_t at = len ? tap_below((uint32_t)len + 1) : 0;

		switch (tap_below(4)) {
		case 0: {
			const char *t = tokens[tap_below(sizeof(tokens) / sizeof(*tokens))];
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
				static const char meta[] = "#'\"`$(){}[]\\\n;|&<>";

				buf[at] = meta[tap_below(sizeof(meta) - 1)];
			}
			break;
		}
	}
	buf[len] = '\0';
	return len;
}

static void test_compact_corpus(void)
{
	load_corpus();
	ok(n_corpus >= 10, "the stripper corpus is present (%zu files)", n_corpus);
	for (size_t k = 0; k < n_corpus; k++) {
		char *a = dup_n(corpus[k], corpus_len[k]);
		size_t la = do_compact(a, corpus_len[k]);
		char *b = dup_n(a, la);
		size_t lb = do_compact(b, la);

		ok(la == lb && !memcmp(a, b, la), "stripping corpus file %zu twice changes nothing", k);
		free(a);
		free(b);
	}
}

static void test_compact_fuzz(void)
{
	size_t cap = 1 << 16, bad = 0, iters = 200000;
	char *buf = malloc(cap), *orig = malloc(cap);

	if (!n_corpus) {
		ok(1, "# SKIP no corpus");
		return;
	}
	for (size_t it = 0; it < iters; it++) {
		size_t k = tap_below((uint32_t)n_corpus), len = corpus_len[k], out, nl_in;

		if (len >= cap / 2)
			continue;
		memcpy(buf, corpus[k], len);
		len = mutate(buf, len, cap / 2);
		memcpy(orig, buf, len + 1);
		nl_in = count_nl(buf, len);
		out = do_compact(buf, len);
		if (out > len || count_nl(buf, out) > nl_in || memchr(buf, '\0', out)) {
			if (bad++ < 3) {
				tap_diag("iteration %zu: %zu bytes became %zu", it, len, out);
				tap_diag_str("input: ", orig, len);
			}
		}
	}
	ok(!bad, "%zu mutated scripts never grow, gain lines or hold a nul once stripped", iters);
	free(buf);
	free(orig);
}

static int bash_n(const char *text, size_t len)
{
	char path[] = "/tmp/ninit-t-build-sh.XXXXXX";
	int fd = mkstemp(path), st = -1, null;
	pid_t pid;

	if (fd < 0 || write(fd, text, len) != (ssize_t)len) {
		close(fd);
		unlink(path);
		return -1;
	}
	close(fd);
	pid = fork();
	if (pid == 0) {
		null = open("/dev/null", O_WRONLY);
		dup2(null, 1);
		dup2(null, 2);
		execl(NG_SHELL, NG_SHELL_ARGV0, "-n", path, (char *)NULL);
		_exit(127);
	}
	waitpid(pid, &st, 0);
	unlink(path);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_compact_syntax(void)
{
	size_t cap = 1 << 16, tested = 0, broke = 0, fixed = 0, iters = 1000;
	char *buf = malloc(cap), *orig = malloc(cap);

	if (!n_corpus || access(NG_SHELL, X_OK)) {
		ok(1, "# SKIP %s", n_corpus ? NG_SHELL " is not executable" : "no corpus");
		free(buf);
		free(orig);
		return;
	}
	for (size_t it = 0; it < iters; it++) {
		size_t k = tap_below((uint32_t)n_corpus), len = corpus_len[k], out;
		int ro, rs;

		memcpy(buf, corpus[k], len);
		len = mutate(buf, len, cap / 2);
		memcpy(orig, buf, len + 1);
		ro = bash_n(orig, len);
		out = do_compact(buf, len);
		rs = bash_n(buf, out);
		tested += ro == 0;
		if (ro == 0 && rs != 0 && broke++ < 3) {
			tap_diag("stripping broke the syntax of a valid script");
			tap_diag_str("input:    ", orig, len);
			tap_diag_str("stripped: ", buf, out);
		}
		if (ro != 0 && rs == 0 && fixed++ < 3) {
			tap_diag("stripping made an invalid script valid");
			tap_diag_str("input:    ", orig, len);
			tap_diag_str("stripped: ", buf, out);
		}
	}
	ok(!broke, "no valid mutated script fails bash -n once stripped (%zu of %zu were valid)",
	   tested, iters);
	ok(!fixed, "no invalid mutated script passes bash -n once stripped");
	free(buf);
	free(orig);
}

struct src_arg {
	const char *text;
	struct src s;
};

static void call_parse(void *p)
{
	struct src_arg *a = p;
	size_t len = strlen(a->text);
	char *body = malloc(len + 1);

	memcpy(body, a->text, len + 1);
	memset(&a->s, 0, sizeof(a->s));
	parse_src(&a->s, "svc", body, len);
}

// LINE with tab and cr spelled out, for test names
static const char *shown(const char *line)
{
	static char buf[256];
	size_t j = 0;

	for (const char *p = line; *p && j + 3 < sizeof(buf); p++) {
		if (*p == '\t' || *p == '\r') {
			buf[j++] = '\\';
			buf[j++] = *p == '\t' ? 't' : 'r';
		} else {
			buf[j++] = *p;
		}
	}
	buf[j] = '\0';
	return buf;
}

static void test_shebang(void)
{
	static const struct {
		const char *line, *want;
	} cases[] = {
		{ "#!/bin/bash", "bash" },
		{ "#!bash", "bash" },
		{ "#! /usr/bin/env python3", "python3" },
		{ "#!\t/usr/bin/perl -w", "perl" },
		{ "#!/usr/bin/env -S bash -e", "bash" },
		{ "#!/usr/bin/env -Sbash -e", "bash" },
		{ "#!/usr/bin/env -S /usr/local/bin/zsh", "zsh" },
		{ "#!/usr/bin/env -u FOO -i perl -w", "perl" },
		{ "#!/usr/bin/env -C / lua", "lua" },
		{ "#!/usr/bin/env A=b C=d zsh", "zsh" },
		{ "#!/usr/bin/env -- awk -f", "awk" },
		{ "#!/bin/bash\r", "bash" },
		{ "#!/usr/bin/env", NULL },
		{ "#!/usr/bin/env -i A=b", NULL },
		{ "#!", NULL },
		{ "#!  \t", NULL },
	};
	const char *zsh = "#!/usr/bin/zsh\n#%type: oneshot\n# note\nls (#i)readme   # case blind\n";
	struct src_arg a;
	char want[512];
	int rc;

	for (size_t k = 0; k < sizeof(cases) / sizeof(*cases); k++) {
		size_t len;
		const char *w = shebang_interp(cases[k].line, strlen(cases[k].line), &len);

		if (!cases[k].want)
			ok(!w, "'%s' names no interpreter", shown(cases[k].line));
		else
			ok(w && len == strlen(cases[k].want) && !memcmp(w, cases[k].want, len),
			   "'%s' names %s", shown(cases[k].line), cases[k].want);
	}

	ok(sh_lexed("sh") && sh_lexed("bash") && sh_lexed("/bin/dash") && sh_lexed("ash") &&
	   sh_lexed("ksh") && sh_lexed("mksh") && sh_lexed("oksh") && sh_lexed("loksh") &&
	   sh_lexed("yash") && sh_lexed("posh"), "sh, bash, dash, ash and the ksh family lex like sh");
	ok(!sh_lexed("zsh") && !sh_lexed("/usr/bin/zsh") && !sh_lexed("fish") && !sh_lexed("bash5") &&
	   !sh_lexed("lua") && !sh_lexed(""), "zsh, fish and other names do not");

	g_dir = "/d";
	g_sh_lexed = 0;
	a.text = zsh;
	rc = capture(call_parse, &a);
	ok(!rc && a.s.script && !strcmp(a.s.script, zsh),
	   "for a shell that does not lex like sh the script is stored as written");
	g_sh_lexed = 1;
	rc = capture(call_parse, &a);
	ok(!rc && a.s.script && !strstr(a.s.script, "(#i)"), "the sh stripper would cut the zsh glob flag");

	a.text = "#!/usr/bin/ruby\n:\n";
	rc = capture(call_parse, &a);
	snprintf(want, sizeof(want), "ninitctl: warning: /d/svc: '#!/usr/bin/ruby' is ignored, "
		 "the script runs under %s\n", NG_SHELL);
	ok(!rc && !strcmp(errtext, want), "a #! line naming another interpreter is warned about");
	a.text = "#!/usr/bin/env -S ninitctl stop foo  \r\n:\n";
	rc = capture(call_parse, &a);
	snprintf(want, sizeof(want), "ninitctl: warning: /d/svc: '#!/usr/bin/env -S ninitctl stop foo' "
		 "is ignored, the script runs under %s\n", NG_SHELL);
	ok(!rc && !strcmp(errtext, want), "the warning quotes the line without trailing blanks");
	a.text = "#!" NG_SHELL "\n:\n";
	rc = capture(call_parse, &a);
	ok(!rc && !*errtext, "the configured shell draws no warning");
	a.text = ":\n#!/usr/bin/python3\n";
	rc = capture(call_parse, &a);
	ok(!rc && !*errtext, "a #! line after the first line is an ordinary comment");

	a.text = "#!/bin/sh\n:\n";
	if (is_name("sh", 2, NG_SHELL) || is_name("sh", 2, NG_SHELL_ARGV0)) {
		ok(1, "# SKIP the configured shell is sh");
		ok(1, "# SKIP the configured shell is sh");
	} else {
		rc = capture(call_parse, &a);
		ok(!rc && !*errtext, "#!/bin/sh draws no warning when the shell lexes like sh");
		g_sh_lexed = 0;
		rc = capture(call_parse, &a);
		ok(!rc && strstr(errtext, "'#!/bin/sh' is ignored"), "but does under a shell that does not");
		g_sh_lexed = 1;
	}
}

// an executable named NAME in DIR, standing in for an interpreter the host may lack
static void fake_interp(const char *dir, const char *name)
{
	char path[512];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd >= 0) {
		(void)!write(fd, "#!/bin/sh\nexit 0\n", 17);
		close(fd);
	}
}

static void test_languages(void)
{
	char dir[] = "/tmp/ninit-t-build-interp.XXXXXX", text[1024], want[512];
	struct src_arg a;
	struct shebang sb;
	const char *line;
	int rc;

	ok(lang_python.claims("python", 6) && lang_python.claims("python3", 7) &&
	   lang_python.claims("python3.14", 10), "python, python3 and python3.14 are python");
	ok(!lang_python.claims("python3-config", 14) && !lang_python.claims("pythonw", 7) &&
	   !lang_python.claims("pyth", 4), "python3-config, pythonw and pyth are not");
	ok(lang_lua.claims("lua", 3) && lang_lua.claims("lua5.4", 6) && lang_lua.claims("luajit", 6) &&
	   lang_lua.claims("luajit-2.1", 10), "lua, lua5.4, luajit and luajit-2.1 are lua");
	ok(!lang_lua.claims("luac", 4) && !lang_lua.claims("luarocks", 8), "luac and luarocks are not");
	ok(lang_perl.claims("perl", 4) && lang_perl.claims("perl5.40.0", 10), "perl and perl5.40.0 are perl");
	ok(!lang_perl.claims("perldoc", 7) && !lang_perl.claims("perl-x", 6), "perldoc and perl-x are not");

	{
		char b[] = "#!/usr/bin/python3\n#%type: oneshot\n# c\nprint(1)  # t\n\n";
		size_t off = (size_t)(strstr(b, "print") - b), n = blank_header(NULL, b, strlen(b), off);

		is_str(b, "\n\n\nprint(1)  # t\n\n", "header lines become empty lines, the body stays as written");
		is_int(n, strlen(b), "and the returned length is the stored length");
	}

	{
		static char name[] = "a\"b\\c";
		struct src s = { .name = name, .interp = "/usr/bin/lua" };

		lang_lua.exec_args(&s);
		ok(s.exec_pre.n == 4 && s.exec_suf.n == 0 && !strcmp(s.exec_pre.v[0], "/usr/bin/lua") &&
		   !strcmp(s.exec_pre.v[1], "-e") && !strcmp(s.exec_pre.v[3], "-e"),
		   "lua runs as INTERP -e CHUNK -e SCRIPT");
		is_str(s.exec_pre.v[2], "io.stdout:setvbuf(\"line\") arg = { [0] = \"a\\\"b\\\\c\" }",
		       "the lua chunk escapes the name");
	}
	{
		static char name[] = "it's\\x";
		struct src s = { .name = name, .interp = "/usr/bin/perl" };

		lang_perl.exec_args(&s);
		ok(s.exec_pre.n == 6 && s.exec_suf.n == 0 && !strcmp(s.exec_pre.v[3], "-e") &&
		   !strcmp(s.exec_pre.v[4], "#line 1") && !strcmp(s.exec_pre.v[5], "-e"),
		   "perl runs as INTERP -e LINE -e '#line 1' -e SCRIPT");
		is_str(s.exec_pre.v[2], "$| = 1; $0 = 'it\\'s\\\\x';", "the perl line escapes the name");
	}
	{
		static char x[] = "-X", dev[] = "dev", name[] = "svc";
		struct src s = { .name = name, .interp = "/usr/bin/python3" };

		strv_push(&s.iargs, x);
		strv_push(&s.iargs, dev);
		lang_python.exec_args(&s);
		ok(s.exec_pre.n == 7 && !strcmp(s.exec_pre.v[1], "-I") && !strcmp(s.exec_pre.v[2], "-B") &&
		   !strcmp(s.exec_pre.v[3], "-u") && !strcmp(s.exec_pre.v[4], "-X") &&
		   !strcmp(s.exec_pre.v[5], "dev") && !strcmp(s.exec_pre.v[6], "-c"),
		   "python runs as INTERP -I -B -u #!-ARGS -c SCRIPT");
		ok(s.exec_suf.n == 1 && !strcmp(s.exec_suf.v[0], "svc"), "with the name after the script");
	}

	g_dir = "/d";
	line = "#!/usr/bin/env -S true -x  y";
	ok(parse_shebang(line, strlen(line), &sb) && sb.len == 4 && !memcmp(sb.prog, "true", 4),
	   "env -S names the program after its options");
	{
		char *p = resolve("svc", &sb), *slash = strrchr(p, '/'), *real;

		ok(p[0] == '/' && !strcmp(slash, "/true") && runnable(p), "env finds true in PATH");
		*slash = '\0';
		real = realpath(p, NULL);
		ok(real && !strcmp(real, p), "in a directory without links, %s", p);
		free(real);
		free(p);
	}

	if (!mkdtemp(dir)) {
		ok(0, "mkdtemp: %s", strerror(errno));
		return;
	}
	fake_interp(dir, "python3");
	fake_interp(dir, "lua5.4");
	fake_interp(dir, "perl");

	snprintf(text, sizeof(text), "#!%s/python3 -X dev\n#%%type: oneshot\n# c\nprint(1)\n", dir);
	a.text = text;
	rc = capture(call_parse, &a);
	if (!ok(!rc && a.s.lang == &lang_python && !strncmp(a.s.interp, dir, strlen(dir)),
		"a #! line naming python selects python"))
		tap_diag("%s", errtext);
	is_str(a.s.script, "\n\n\nprint(1)\n", "its header is blanked, its body stored as written");
	ok(a.s.exec_pre.n == 7 && a.s.exec_suf.n == 1, "the #! arguments reach its argv");
	ok(!a.s.stripped, "its script is not stripped");

	snprintf(text, sizeof(text), "#!%s/lua5.4\nprint(1) -- c\n", dir);
	rc = capture(call_parse, &a);
	ok(!rc && a.s.lang == &lang_lua, "a #! line naming lua5.4 selects lua");
	snprintf(text, sizeof(text), "#!%s/perl -w\nprint 1;\n", dir);
	rc = capture(call_parse, &a);
	ok(!rc && a.s.lang == &lang_perl && a.s.iargs.n == 1, "a #! line naming perl selects perl");

	snprintf(text, sizeof(text), "#!%s/python3 -b -d -E -i -O -P -q -R -s -S -v\nprint(1)\n", dir);
	rc = capture(call_parse, &a);
	if (!ok(rc == 1001 && !strcmp(errtext, "ninitctl: /d/svc: 17 interpreter arguments, the maximum is 16\n"),
		"more than 16 interpreter arguments are refused"))
		tap_diag("%d %s", rc, errtext);

	snprintf(text, sizeof(text), "#!%s/missing/python3\nprint(1)\n", dir);
	rc = capture(call_parse, &a);
	snprintf(want, sizeof(want), "ninitctl: /d/svc: %s/missing/python3: No such file or directory\n", dir);
	if (!ok(rc == 1001 && !strcmp(errtext, want), "a missing interpreter fails the build"))
		tap_diag("%d %s", rc, errtext);
	a.text = "#!/usr/bin/env python9.99\nprint(1)\n";
	rc = capture(call_parse, &a);
	if (!ok(rc == 1001 && strstr(errtext, "ninitctl: /d/svc: 'python9.99' is not in /"),
		"an interpreter env cannot find fails the build"))
		tap_diag("%d %s", rc, errtext);
	a.text = "#!bin/python3\nprint(1)\n";
	rc = capture(call_parse, &a);
	if (!ok(rc == 1001 && !strcmp(errtext, "ninitctl: /d/svc: 'bin/python3' is not an absolute path\n"),
		"a relative interpreter path fails the build"))
		tap_diag("%d %s", rc, errtext);

	{
		static const struct {
			const char *interp, *args, *word, *why;
		} v[] = {
			{ "python3", "foo.py", "foo.py", ARG_SCRIPT },
			{ "python3", "-m http.server", "-m", ARG_SCRIPT },
			{ "python3", "-Bc pass", "-Bc", ARG_SCRIPT },
			{ "python3", "-", "-", ARG_SCRIPT },
			{ "python3", "-u --", "--", ARG_SCRIPT },
			{ "python3", "-X dev -W", "-W", ARG_SCRIPT },
			{ "python3", "--check-hash-based-pycs", "--check-hash-based-pycs", ARG_SCRIPT },
			{ "python3", "-V", "-V", ARG_EXIT },
			{ "python3", "--help-env", "--help-env", ARG_EXIT },
			{ "python3", "-W error -X dev -Xutf8 --check-hash-based-pycs never -OO", NULL, NULL },
			{ "lua5.4", "foo.lua", "foo.lua", ARG_SCRIPT },
			{ "lua5.4", "-e x=1", "-e", ARG_CODE },
			{ "lua5.4", "-E --", "--", ARG_SCRIPT },
			{ "lua5.4", "-l", "-l", ARG_EXIT },
			{ "lua5.4", "-b", "-b", ARG_SCRIPT },
			{ "lua5.4", "-E -W -l string -lstring -j off -O3", NULL, NULL },
			{ "perl", "foo.pl", "foo.pl", ARG_SCRIPT },
			{ "perl", "-wle", "-wle", ARG_CODE },
			{ "perl", "-x", "-x", ARG_SCRIPT },
			{ "perl", "-I", "-I", ARG_SCRIPT },
			{ "perl", "-c", "-c", ARG_EXIT },
			{ "perl", "-dt", "-dt", ARG_EXIT },
			{ "perl", "-n", "-n", ARG_LOOP },
			{ "perl", "-F:", "-F:", ARG_LOOP },
			{ "perl", "-wT -0777 -0xFF -l012 -CSDA -Mstrict -I /tmp -i.bak -d:Foo", NULL, NULL },
		};

		for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
			snprintf(text, sizeof(text), "#!%s/%s %s\n:\n", dir, v[k].interp, v[k].args);
			a.text = text;
			rc = capture(call_parse, &a);
			if (v[k].why)
				snprintf(want, sizeof(want), "ninitctl: /d/svc: #! argument '%s' %s\n",
					 v[k].word, v[k].why);
			if (!ok(v[k].why ? rc == 1001 && !strcmp(errtext, want) : !rc,
				"%s %s is %s", v[k].interp, v[k].args, v[k].why ? "refused" : "accepted"))
				tap_diag("%d %s", rc, errtext);
		}
	}

	snprintf(text, sizeof(text), "%s/python3", dir);
	unlink(text);
	snprintf(text, sizeof(text), "%s/lua5.4", dir);
	unlink(text);
	snprintf(text, sizeof(text), "%s/perl", dir);
	unlink(text);
	rmdir(dir);
}

int main(void)
{
	test_parse_ms();
	test_helpers();
	test_shebang();
	test_languages();
	test_compact_corpus();
	test_compact_fuzz();
	test_compact_syntax();
	return tap_done();
}
