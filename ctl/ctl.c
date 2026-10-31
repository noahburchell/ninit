#include "ctl.h"
#include "../help/io.h"
#include "../src/nctl.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CTL_LINE 4096

struct sty {
	const char* dim;
	const char* green;
	const char* yellow;
	const char* red;
	const char* reset;
	const char* bar;
	const char* none;
};

static const struct sty sty_tty = {
	"\033[2m",
	"\033[32m",
	"\033[33m",
	"\033[1;31m",
	"\033[0m",
	"\u2500",
	"\u2014",
};

static const struct sty sty_plain = { "", "", "", "", "", "-", "-" };

struct srow {
	char* dup;
	char* name;
	char* state;
	char* want;
	char* pid;
};

struct stab {
	struct srow* v;
	size_t       n, cap;
	bool         raw;
};

// NAME STATE wanted|stopped pid N, checked without disturbing the line
static bool st_shaped(const char* s) {
	const char* tok3 = NULL;
	uint8_t i;

	for(i = 0; i < 5; i++) {
		while(*s == ' ') {
			s++;
		}

		if(!*s) {
			return false;
		}

		if(i == 3) {
			tok3 = s;
		}

		while(*s && *s != ' ') {
			s++;
		}
	}

	while(*s == ' ') {
		s++;
	}

	return !*s && !strncmp(tok3, "pid ", 4);
}

static void st_split(struct srow* r) {
	char** fld[5] = { &r->name, &r->state, &r->want, NULL, &r->pid };
	char* q = r->dup;
	uint8_t i;

	for(i = 0; i < 5; i++) {
		while(*q == ' ') {
			q++;
		}

		if(fld[i]) {
			*fld[i] = q;
		}

		while(*q && *q != ' ') {
			q++;
		}

		if(*q) {
			*q++ = '\0';
		}
	}
}

static bool stab_add(struct stab* t, const char* line) {
	struct srow r = { 0 };

	if(t->n == t->cap) {
		size_t cap = t->cap ? t->cap * 2 : 64;
		struct srow* v = realloc(t->v, cap * sizeof(*v));

		if(!v) {
			return false;
		}

		t->v = v;
		t->cap = cap;
	}

	r.dup = strdup(line);
	if(!r.dup) {
		return false;
	}

	if(!st_shaped(line)) {
		t->raw = true;
	}

	t->v[t->n++] = r;

	return true;
}

static const char* state_colour(const struct sty* st, const char* state) {
	if(!strcmp(state, "up")) {
		return st->green;
	}

	if(!strcmp(state, "failed")) {
		return st->red;
	}

	if(!strcmp(state, "starting") || !strcmp(state, "down")) {
		return st->yellow;
	}

	return st->dim;
}

static bool stab_print(struct stab* t) {
	const struct sty* st = isatty(STDOUT_FILENO) ? &sty_tty : &sty_plain;
	size_t i, wn = 7, ws = 5, wp = 3, wi = 1, rule;
	bool held = false, table = false;
	char idx[24];

	if(!t->n) {
		return false;
	}

	if(t->raw) {
		for(i = 0; i < t->n; i++) {
			printf("%s\n", t->v[i].dup);
		}

		goto done;
	}

	table = true;

	wi = (size_t)snprintf(idx, sizeof(idx), "%zu", t->n - 1);
	for(i = 0; i < t->n; i++) {
		size_t l;

		st_split(&t->v[i]);
		l = strlen(t->v[i].name);

		if(l > wn) {
			wn = l;
		}

		l = strlen(t->v[i].state);
		if(l > ws) {
			ws = l;
		}

		l = strcmp(t->v[i].pid, "0") ? strlen(t->v[i].pid) : 1;
		if(l > wp) {
			wp = l;
		}

		if(strcmp(t->v[i].want, "wanted")) {
			held = true;
		}
	}

	printf("%s  %*s  %-*s  %-*s  %*s%s%s\n", st->dim, (int)wi, "#", (int)wn,
	       "service", (int)ws, "state", (int)wp, "pid", held ? "  held" : "",
	       st->reset);
	fputs(st->dim, stdout);
	fputs("  ", stdout);
	rule = wi + wn + ws + wp + 6 + (held ? 6 : 0);
	for(i = 0; i < rule; i++) {
		fputs(st->bar, stdout);
	}

	printf("%s\n", st->reset);

	for(i = 0; i < t->n; i++) {
		const struct srow* r = &t->v[i];
		const char* col = state_colour(st, r->state);
		bool gone = !strcmp(r->pid, "0");
		size_t pl = gone ? 1 : strlen(r->pid);

		printf("  %s%*zu%s  %-*s  ", st->dim, (int)wi, i, st->reset,
		       (int)wn, r->name);
		printf("%s%s%s%*s  ", col, r->state, st->reset,
		       (int)(ws - strlen(r->state)), "");
		printf("%*s", (int)(wp - pl), "");
		if(gone) {
			printf("%s%s%s", st->dim, st->none, st->reset);
		} else {
			fputs(r->pid, stdout);
		}

		if(held && strcmp(r->want, "wanted")) {
			fputs("  yes", stdout);
		}

		putchar('\n');
	}

done:
	for(i = 0; i < t->n; i++) {
		free(t->v[i].dup);
	}

	free(t->v);
	t->v = NULL;
	t->n = t->cap = 0;
	t->raw = false;

	return table;
}

// the tag of a "[sss.mmm] TAG > text" line, or NULL
static const char* log_tag(const char* l) {
	const char* p = l[0] == '[' ? strchr(l, ']') : NULL;

	if(!p || p[1] != ' ' || strnlen(p + 2, 7) < 7 || memcmp(p + 6, " > ", 3)) {
		return NULL;
	}

	return p + 2;
}

// the tag in the colour ninit gives it on the console
static void log_print(const char* l, bool errors, bool tty) {
	static const char* const col[][2] = {
		{ "DONE", "\033[32m" },
		{ "NOTE", "\033[36m" },
		{ "WAIT", "\033[1;34m" },
		{ "WARN", "\033[33m" },
		{ "FAIL", "\033[31m" },
	};
	const char* t = log_tag(l);
	size_t k;

	if(errors && (!t || (memcmp(t, "WARN", 4) && memcmp(t, "FAIL", 4)))) {
		return;
	}

	if(tty && !t && !strncmp(l, "Welcome to ", 11)) {
		const char* e = NULL;
		const char* q = l + 11;

		while((q = strstr(q, "! ("))) {
			e = q++;
		}

		if(e) {
			printf("Welcome to \033[1m%.*s\033[0m%s\n", (int)(e - l - 11), l + 11, e);
			return;
		}
	}

	if(tty && t) {
		for(k = 0; k < sizeof(col) / sizeof(*col); k++) {
			if(!memcmp(t, col[k][0], 4)) {
				printf("%.*s%s%.4s\033[0m%s\n", (int)(t - l), l, col[k][1], t, t + 4);
				return;
			}
		}
	}

	puts(l);
}

static int ctl_connect(void) {
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if(fd < 0) {
		fprintf(stderr, "ninitctl: socket: %s\n", strerror(errno));
		return -1;
	}

	memcpy(sa.sun_path, NINIT_CTL_SOCK, sizeof(NINIT_CTL_SOCK));
	if(connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "ninitctl: %s: %s\n", NINIT_CTL_SOCK, strerror(errno));
		if(errno == ENOENT) {
			fprintf(stderr, "ninitctl: pid 1 is not ninit\n");
		} else if(errno == EACCES) {
			fprintf(stderr, "ninitctl: permission denied\n");
		}

		close(fd);
		return -1;
	}

	return fd;
}

int cmd_ctl(const char* verb, int argc, char** argv) {
	char buf[CTL_LINE * 2], line[CTL_LINE];
	const char* name = NULL;
	struct stab tab = { 0 };
	size_t held = 0, at;
	int fd, k, rc = NCTL_EXIT_USAGE;
	bool endopts = false, seen_end = false;
	bool tabular = !strcmp(verb, "status"), log = !strcmp(verb, "log");
	bool errors = false, watch = false, boot = false, tty = isatty(STDOUT_FILENO);
	ssize_t n;

	for(k = 0; k < argc; k++) {
		if(!endopts && !strcmp(argv[k], "--")) {
			endopts = true;
			continue;
		}

		if(!endopts && log && (!strcmp(argv[k], "-e") || !strcmp(argv[k], "--error"))) {
			errors = true;
			continue;
		}

		if(!endopts && log && (!strcmp(argv[k], "-w") || !strcmp(argv[k], "--watch"))) {
			watch = true;
			continue;
		}

		if(!endopts && log && (!strcmp(argv[k], "-t") || !strcmp(argv[k], "--time"))) {
			boot = true;
			continue;
		}

		if(!endopts && argv[k][0] == '-' && argv[k][1]) {
			fprintf(stderr, "ninitctl: %s: unknown option '%s'\n", verb, argv[k]);
			return 2;
		}

		if(name) {
			fprintf(stderr, "ninitctl: %s takes at most one service name\n", verb);
			return 2;
		}

		name = argv[k];
	}

	if(!name && strcmp(verb, "status") && strcmp(verb, "resume")
	   && strcmp(verb, "reload") && strcmp(verb, "log")) {
		fprintf(stderr, "ninitctl: %s needs a service name\n", verb);
		return NCTL_EXIT_USAGE;
	}

	if(name && (!strcmp(verb, "resume") || !strcmp(verb, "reload") || !strcmp(verb, "log"))) {
		fprintf(stderr, "ninitctl: %s takes no arguments\n", verb);
		return NCTL_EXIT_USAGE;
	}

	if(boot && (errors || watch)) {
		fprintf(stderr, "ninitctl: log: '-t' excludes '-e' and '-w'\n");
		return NCTL_EXIT_USAGE;
	}

	if(watch) {
		name = "watch";
	}

	if(boot) {
		name = "time";
	}

	at = (size_t)snprintf(line, sizeof(line), "%s%s%s\n", verb, name ? " " : "",
			      name ? name : "");
	if(at >= sizeof(line)) {
		fprintf(stderr, "ninitctl: %s: name is too long\n", verb);
		return NCTL_EXIT_USAGE;
	}

	fd = ctl_connect();
	if(fd < 0) {
		return NCTL_EXIT_FAIL;
	}

	if(write_all(fd, line, at) < 0) {
		fprintf(stderr, "ninitctl: write: %s\n", strerror(errno));
		close(fd);
		return NCTL_EXIT_FAIL;
	}

	for(;;) {
		char* p = buf;
		char* nl;

		n = read(fd, buf + held, sizeof(buf) - held - 1);
		if(n < 0 && errno == EINTR) {
			continue;
		}

		if(n <= 0) {
			break;
		}

		held += (size_t)n;
		buf[held] = '\0';
		while((nl = memchr(p, '\n', held - (size_t)(p - buf))) != NULL) {
			*nl = '\0';
			if(NCTL_IS_DATA(p)) {
				if(boot) {
					printf("%s\n", p + NCTL_TAG_LEN);
				} else if(log) {
					log_print(p + NCTL_TAG_LEN, errors, tty);
				} else if(!tabular || !stab_add(&tab, p + NCTL_TAG_LEN)) {
					printf("%s\n", p + NCTL_TAG_LEN);
				}
			} else if(NCTL_IS_OK(p)) {
				bool shown = stab_print(&tab);

				// for one service the trailing line only repeats its name
				if(!log && (!tabular || !name)) {
					printf("%s%s\n", shown ? "\n" : "",
					       p + NCTL_TAG_LEN);
				}

				rc = NCTL_EXIT_OK;
				seen_end = true;
			} else if(NCTL_IS_ERR(p)) {
				stab_print(&tab);
				fprintf(stderr, "ninitctl: %s\n", p + NCTL_TAG_LEN);
				rc = NCTL_EXIT_FAIL;
				seen_end = true;
			} else if(*p) {
				fprintf(stderr, "ninitctl: malformed reply: %s\n", p);
			}

			p = nl + 1;
		}

		if(watch) {
			fflush(stdout);
		}

		held -= (size_t)(p - buf);
		memmove(buf, p, held);
		if(held >= sizeof(buf) - 1) {
			stab_print(&tab);
			fprintf(stderr, "ninitctl: reply record too long\n");
			close(fd);
			return 1;
		}
	}

	stab_print(&tab);
	if(n < 0) {
		fprintf(stderr, "ninitctl: read: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	close(fd);

	if(!seen_end) {
		fprintf(stderr, "ninitctl: %s: connection closed without a result\n", verb);
		return 1;
	}

	return rc;
}
