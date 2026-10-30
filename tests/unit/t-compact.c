#include "../../ctl/parser/parser.h"
#include "../../ctl/util.h"
#include "ngraph.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tap.h"

static char* dup_n(const char* s, size_t n) {
	char* p = malloc(n + 1);

	memcpy(p, s, n);
	p[n] = '\0';

	return p;
}

static size_t count_nl(const char* s, size_t n) {
	size_t c = 0;

	for(size_t k = 0; k < n; k++) {
		c += s[k] == '\n';
	}

	return c;
}

static size_t do_compact(char* s, size_t n) {
	return compact(s, n, n, "fuzz");
}

static char** corpus;
static size_t* corpus_len;
static size_t n_corpus;

static void load_corpus(void) {
	const char* top = getenv("top_srcdir");
	char dir[4096];
	struct dirent** ents;
	int ne;

	if(!top) {
		top = ".";
	}

	snprintf(dir, sizeof(dir), "%s/tests/compact", top);
	ne = scandir(dir, &ents, NULL, alphasort);
	if(ne < 0) {
		return;
	}

	corpus = calloc((size_t)ne, sizeof(*corpus));
	corpus_len = calloc((size_t)ne, sizeof(*corpus_len));
	for(int k = 0; k < ne; k++) {
		char path[8192];
		size_t len;

		if(ents[k]->d_name[0] == '.' || strstr(ents[k]->d_name, ".want")) {
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

static const char* const tokens[] = {
	"#",
	" # c\n",
	"<<EOF\n",
	"\nEOF\n",
	"<<-X\n\tX\n",
	"<<'Q'\nq\nQ\n",
	"\\\n",
	"\\",
	"$(",
	")",
	"(",
	"'",
	"\"",
	"`",
	"${",
	"}",
	"{",
	"case x in",
	" in ",
	"esac",
	";;",
	";&",
	"[[",
	"]]",
	"=~",
	"((",
	"))",
	"$((",
	"\n",
	" ",
	"\t",
	"a=(",
	"[",
	"]",
	"=",
	"@(",
	"!(",
	"time",
	"coproc",
	"for",
	"do",
	"done",
	"<(",
	">(",
	"<<<",
	"&",
	"|",
	";",
	"$'",
	"#%x: y\n",
	"\r",
};

static size_t mutate(char* buf, size_t len, size_t cap) {
	unsigned ops = 1 + tap_below(3);

	for(unsigned o = 0; o < ops; o++) {
		size_t at = len ? tap_below((uint32_t)len + 1) : 0;

		switch(tap_below(5)) {
			case 0: {
				const char* t = tokens[tap_below(sizeof(tokens) / sizeof(*tokens))];
				size_t tl = strlen(t);

				if(len + tl >= cap) {
					break;
				}

				memmove(buf + at + tl, buf + at, len - at);
				memcpy(buf + at, t, tl);
				len += tl;
				break;
			}

			case 1: {
				size_t dl = 1 + tap_below(8);

				if(at + dl > len) {
					dl = len - at;
				}

				memmove(buf + at, buf + at + dl, len - at - dl);
				len -= dl;
				break;
			}

			case 2:
				if(at < len) {
					buf[at] = (char)(' ' + tap_below(95));
				}

				break;
			case 3:
				// a construct cut off by the end of the file
				len = at;
				break;
			default:
				if(at < len) {
					static const char meta[] = "#'\"`$(){}[]\\\n;|&<>";

					buf[at] = meta[tap_below(sizeof(meta) - 1)];
				}

				break;
		}
	}

	buf[len] = '\0';

	return len;
}

static void test_compact_corpus(void) {
	load_corpus();
	ok(n_corpus >= 10, "the stripper corpus is present (%zu files)", n_corpus);
	for(size_t k = 0; k < n_corpus; k++) {
		char* a = dup_n(corpus[k], corpus_len[k]);
		size_t la = do_compact(a, corpus_len[k]);
		char* b = dup_n(a, la);
		size_t lb = do_compact(b, la);

		ok(la == lb && !memcmp(a, b, la), "stripping corpus file %zu twice changes nothing", k);
		free(a);
		free(b);
	}
}

static void test_compact_fuzz(void) {
	size_t cap = 1 << 16, bad = 0, iters = 200000;
	char* buf = malloc(cap);
	char* orig = malloc(cap);

	if(!n_corpus) {
		ok(1, "# SKIP no corpus");
		return;
	}

	for(size_t it = 0; it < iters; it++) {
		size_t k = tap_below((uint32_t)n_corpus), len = corpus_len[k], out, nl_in;
		char* tight;

		if(len >= cap / 2) {
			continue;
		}

		memcpy(buf, corpus[k], len);
		len = mutate(buf, len, cap / 2);
		memcpy(orig, buf, len + 1);
		nl_in = count_nl(buf, len);
		// the script fills its buffer, so a sanitizer sees a read past the terminating nul
		tight = malloc(len + 1);
		memcpy(tight, buf, len + 1);
		out = do_compact(tight, len);
		if(out > len || count_nl(tight, out) > nl_in || memchr(tight, '\0', out)) {
			if(bad++ < 3) {
				tap_diag("iteration %zu: %zu bytes became %zu", it, len, out);
				tap_diag_str("input: ", orig, len);
			}
		}

		free(tight);
	}

	ok(!bad, "%zu mutated scripts never grow, gain lines or hold a nul once stripped", iters);
	free(buf);
	free(orig);
}

static int bash_n(const char* text, size_t len) {
	char path[] = "/tmp/ninit-t-compact-sh.XXXXXX";
	int fd = mkstemp(path), st = -1, null;
	pid_t pid;

	if(fd < 0 || write(fd, text, len) != (ssize_t)len) {
		close(fd);
		unlink(path);
		return -1;
	}

	close(fd);
	pid = fork();
	if(pid == 0) {
		null = open("/dev/null", O_WRONLY);
		dup2(null, 1);
		dup2(null, 2);
		execl(NG_SHELL, NG_SHELL_ARGV0, "-n", path, (char*)NULL);
		_exit(127);
	}

	waitpid(pid, &st, 0);
	unlink(path);

	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_compact_syntax(void) {
	size_t cap = 1 << 16, tested = 0, broke = 0, fixed = 0, iters = 1000;
	char* buf = malloc(cap);
	char* orig = malloc(cap);

	if(!n_corpus || access(NG_SHELL, X_OK)) {
		ok(1, "# SKIP %s", n_corpus ? NG_SHELL " is not executable" : "no corpus");
		free(buf);
		free(orig);
		return;
	}

	for(size_t it = 0; it < iters; it++) {
		size_t k = tap_below((uint32_t)n_corpus), len = corpus_len[k], out;
		int ro, rs;

		memcpy(buf, corpus[k], len);
		len = mutate(buf, len, cap / 2);
		memcpy(orig, buf, len + 1);
		ro = bash_n(orig, len);
		out = do_compact(buf, len);
		rs = bash_n(buf, out);
		tested += ro == 0;
		if(ro == 0 && rs != 0 && broke++ < 3) {
			tap_diag("stripping broke the syntax of a valid script");
			tap_diag_str("input:    ", orig, len);
			tap_diag_str("stripped: ", buf, out);
		}

		if(ro != 0 && rs == 0 && fixed++ < 3) {
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

int main(void) {
	test_compact_corpus();
	test_compact_fuzz();
	test_compact_syntax();

	return tap_done();
}
