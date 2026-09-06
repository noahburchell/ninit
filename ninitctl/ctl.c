#include "ctl.h"
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
	const char *dim, *green, *yellow, *red, *reset, *bar, *none;
};

static const struct sty sty_tty = {
	"\033[2m", "\033[32m", "\033[33m", "\033[1;31m", "\033[0m", "\u2500", "\u2014",
};

static const struct sty sty_plain = { "", "", "", "", "", "-", "-" };

struct srow {
	char *dup;
	char *name, *state, *want, *pid;
};

struct stab {
	struct srow *v;
	size_t n, cap;
	int raw;
};

// NAME STATE wanted|stopped pid N, checked without disturbing the line
static int st_shaped(const char *s)
{
	const char *tok3 = NULL;
	int i;

	for (i = 0; i < 5; i++) {
		while (*s == ' ')
			s++;
		if (!*s)
			return 0;
		if (i == 3)
			tok3 = s;
		while (*s && *s != ' ')
			s++;
	}
	while (*s == ' ')
		s++;
	return !*s && !strncmp(tok3, "pid ", 4);
}

// splitting is destructive, so it waits until every line is known to be shaped
static void st_split(struct srow *r)
{
	char **fld[5] = { &r->name, &r->state, &r->want, NULL, &r->pid };
	char *q = r->dup;
	int i;

	for (i = 0; i < 5; i++) {
		while (*q == ' ')
			q++;
		if (fld[i])
			*fld[i] = q;
		while (*q && *q != ' ')
			q++;
		if (*q)
			*q++ = '\0';
	}
}

static int stab_add(struct stab *t, const char *line)
{
	struct srow r = { 0 };

	if (t->n == t->cap) {
		size_t cap = t->cap ? t->cap * 2 : 64;
		struct srow *v = realloc(t->v, cap * sizeof(*v));

		if (!v)
			return 0;
		t->v = v;
		t->cap = cap;
	}
	r.dup = strdup(line);
	if (!r.dup)
		return 0;
	if (!st_shaped(line))
		t->raw = 1;
	t->v[t->n++] = r;
	return 1;
}

static const char *state_colour(const struct sty *st, const char *state)
{
	if (!strcmp(state, "up"))
		return st->green;
	if (!strcmp(state, "failed"))
		return st->red;
	if (!strcmp(state, "starting") || !strcmp(state, "down"))
		return st->yellow;
	return st->dim;
}

static int stab_print(struct stab *t)
{
	const struct sty *st = isatty(STDOUT_FILENO) ? &sty_tty : &sty_plain;
	size_t i, wn = 7, ws = 5, wp = 3, wi = 1, rule;
	int held = 0, table = 0;
	char idx[24];

	if (!t->n)
		return 0;
	if (t->raw) {
		for (i = 0; i < t->n; i++)
			printf("%s\n", t->v[i].dup);
		goto done;
	}
	table = 1;

	wi = (size_t)snprintf(idx, sizeof(idx), "%zu", t->n - 1);
	for (i = 0; i < t->n; i++) {
		size_t l;

		st_split(&t->v[i]);
		l = strlen(t->v[i].name);

		if (l > wn)
			wn = l;
		l = strlen(t->v[i].state);
		if (l > ws)
			ws = l;
		l = strcmp(t->v[i].pid, "0") ? strlen(t->v[i].pid) : 1;
		if (l > wp)
			wp = l;
		if (strcmp(t->v[i].want, "wanted"))
			held = 1;
	}

	printf("%s  %*s  %-*s  %-*s  %*s%s%s\n", st->dim, (int)wi, "#", (int)wn,
	       "service", (int)ws, "state", (int)wp, "pid", held ? "  held" : "",
	       st->reset);
	fputs(st->dim, stdout);
	fputs("  ", stdout);
	rule = wi + wn + ws + wp + 6 + (held ? 6 : 0);
	for (i = 0; i < rule; i++)
		fputs(st->bar, stdout);
	printf("%s\n", st->reset);

	for (i = 0; i < t->n; i++) {
		const struct srow *r = &t->v[i];
		const char *col = state_colour(st, r->state);
		int gone = !strcmp(r->pid, "0");
		size_t pl = gone ? 1 : strlen(r->pid);

		printf("  %s%*zu%s  %-*s  ", st->dim, (int)wi, i, st->reset,
		       (int)wn, r->name);
		printf("%s%s%s%*s  ", col, r->state, st->reset,
		       (int)(ws - strlen(r->state)), "");
		printf("%*s", (int)(wp - pl), "");
		if (gone)
			printf("%s%s%s", st->dim, st->none, st->reset);
		else
			fputs(r->pid, stdout);
		if (held && strcmp(r->want, "wanted"))
			fputs("  yes", stdout);
		putchar('\n');
	}

done:
	for (i = 0; i < t->n; i++)
		free(t->v[i].dup);
	free(t->v);
	t->v = NULL;
	t->n = t->cap = 0;
	t->raw = 0;
	return table;
}

static int ctl_connect(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0) {
		fprintf(stderr, "ninitctl: socket: %s\n", strerror(errno));
		return -1;
	}
	memcpy(sa.sun_path, NINIT_CTL_SOCK, sizeof(NINIT_CTL_SOCK));
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "ninitctl: %s: %s\n", NINIT_CTL_SOCK, strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr, "ninitctl: pid 1 is not ninit, or it is too old to listen\n");
		else if (errno == EACCES)
			fprintf(stderr, "ninitctl: only root may control services\n");
		close(fd);
		return -1;
	}
	return fd;
}

static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t k = write(fd, p, n);

		if (k > 0) {
			p += k;
			n -= (size_t)k;
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

int cmd_ctl(const char *verb, int argc, char **argv)
{
	char buf[CTL_LINE * 2], line[CTL_LINE];
	const char *name = NULL;
	struct stab tab = { 0 };
	size_t held = 0, at;
	int fd, k, endopts = 0, rc = NCTL_EXIT_USAGE, seen_end = 0;
	int tabular = !strcmp(verb, "status");
	ssize_t n;

	for (k = 0; k < argc; k++) {
		if (!endopts && !strcmp(argv[k], "--")) {
			endopts = 1;
			continue;
		}
		if (!endopts && argv[k][0] == '-' && argv[k][1]) {
			fprintf(stderr, "ninitctl: %s: unknown option '%s'\n", verb, argv[k]);
			return 2;
		}
		if (name) {
			fprintf(stderr, "ninitctl: %s takes at most one service name\n", verb);
			return 2;
		}
		name = argv[k];
	}

	if (!name && strcmp(verb, "status") && strcmp(verb, "resume") &&
	    strcmp(verb, "reload") && strcmp(verb, "log")) {
		fprintf(stderr, "ninitctl: %s needs a service name\n", verb);
		return NCTL_EXIT_USAGE;
	}
	if (name && (!strcmp(verb, "resume") || !strcmp(verb, "reload") ||
		     !strcmp(verb, "log"))) {
		fprintf(stderr, "ninitctl: %s takes no arguments\n", verb);
		return NCTL_EXIT_USAGE;
	}

	at = (size_t)snprintf(line, sizeof(line), "%s%s%s\n", verb, name ? " " : "",
			      name ? name : "");
	if (at >= sizeof(line)) {
		fprintf(stderr, "ninitctl: %s: name is too long\n", verb);
		return NCTL_EXIT_USAGE;
	}

	fd = ctl_connect();
	if (fd < 0)
		return NCTL_EXIT_FAIL;
	if (write_all(fd, line, at) < 0) {
		fprintf(stderr, "ninitctl: write: %s\n", strerror(errno));
		close(fd);
		return NCTL_EXIT_FAIL;
	}

	while ((n = read(fd, buf + held, sizeof(buf) - held - 1)) > 0) {
		char *p = buf, *nl;

		held += (size_t)n;
		buf[held] = '\0';
		while ((nl = memchr(p, '\n', held - (size_t)(p - buf))) != NULL) {
			*nl = '\0';
			if (NCTL_IS_DATA(p)) {
				if (!tabular || !stab_add(&tab, p + NCTL_TAG_LEN))
					printf("%s\n", p + NCTL_TAG_LEN);
			} else if (NCTL_IS_OK(p)) {
				int shown = stab_print(&tab);

				// for one service the trailing line only repeats its name
				if (!tabular || !name)
					printf("%s%s\n", shown ? "\n" : "",
					       p + NCTL_TAG_LEN);
				rc = NCTL_EXIT_OK;
				seen_end = 1;
			} else if (NCTL_IS_ERR(p)) {
				stab_print(&tab);
				fprintf(stderr, "ninitctl: %s\n", p + NCTL_TAG_LEN);
				rc = NCTL_EXIT_FAIL;
				seen_end = 1;
			} else if (*p) {
				fprintf(stderr, "ninitctl: unframed reply: %s\n", p);
			}
			p = nl + 1;
		}
		held -= (size_t)(p - buf);
		memmove(buf, p, held);
		if (held >= sizeof(buf) - 1) {
			stab_print(&tab);
			fprintf(stderr, "ninitctl: reply record too long\n");
			close(fd);
			return 1;
		}
	}
	stab_print(&tab);
	if (n < 0) {
		fprintf(stderr, "ninitctl: read: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	if (!seen_end) {
		fprintf(stderr, "ninitctl: %s: pid 1 closed the connection without a result; "
			"the operation may still be in progress\n", verb);
		return 1;
	}
	return rc;
}
