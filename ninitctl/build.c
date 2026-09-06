#include "build.h"
#include "../src/ngraph.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

struct strv {
	char **v;
	uint32_t n, cap;
};

struct src {
	char *name;
	const char *script;
	uint8_t type;
	int have_type;
	uint8_t onfail;
	int have_onfail;
	uint8_t restart;
	uint16_t notify;
	uint32_t start_ms;
	uint32_t stop_ms;
	uint16_t retry_ms;
	uint8_t start_tries;
	uint8_t pflags;
	struct strv depon;
	struct strv depof;
};

struct edge {
	uint32_t a, b;
};

static const char *g_dir;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void die(const char *fmt, ...)
{
	va_list ap;

	fputs("ninitctl: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void usage_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void usage_die(const char *fmt, ...)
{
	va_list ap;

	fputs("ninitctl: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

static void *xmalloc(size_t n)
{
	void *p = calloc(1, n ? n : 1);

	if (!p)
		die("out of memory");
	return p;
}

static void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);

	if (!q)
		die("out of memory");
	return q;
}

static void strv_push(struct strv *s, char *v)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 4;
		s->v = xrealloc(s->v, s->cap * sizeof(*s->v));
	}
	s->v[s->n++] = v;
}

static char *slurp(const char *path, size_t *len)
{
	struct stat st;
	char *buf;
	int fd;
	ssize_t got, pos = 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		die("open %s: %s", path, strerror(errno));
	if (fstat(fd, &st) < 0)
		die("stat %s: %s", path, strerror(errno));
	if ((uint64_t)st.st_size > NG_MAX_SRC)
		die("%s: is %llu bytes; a service file may not exceed %u",
		    path, (unsigned long long)st.st_size, NG_MAX_SRC);

	buf = xmalloc((size_t)st.st_size + 1);
	while (pos < st.st_size) {
		got = read(fd, buf + pos, (size_t)st.st_size - pos);
		if (got < 0)
			die("read %s: %s", path, strerror(errno));
		if (!got)
			break;
		pos += got;
	}
	close(fd);

	buf[pos] = '\0';
	*len = (size_t)pos;
	return buf;
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

static void split_into(struct strv *out, char *val, int comma)
{
	char *p = val;

	while (*p) {
		char *start;

		while (*p == ' ' || *p == '\t' || (comma && *p == ','))
			p++;
		if (!*p)
			break;
		start = p;
		while (*p && *p != ' ' && *p != '\t' && !(comma && *p == ','))
			p++;
		if (*p)
			*p++ = '\0';
		strv_push(out, start);
	}
}

static uint32_t parse_ms(const char *val, const char *fname, const char *key)
{
	char *end;
	unsigned long long v, mul = 1;

	errno = 0;
	v = strtoull(val, &end, 10);
	if (*val == '-' || errno || end == val)
		die("%s/%s: %s:%s is not a duration", g_dir, fname, key, val);
	if (!strcmp(end, "ms") || !*end)
		mul = 1;
	else if (!strcmp(end, "s"))
		mul = 1000;
	else if (!strcmp(end, "m"))
		mul = 60000;
	else if (!strcmp(end, "h"))
		mul = 3600000;
	else
		die("%s/%s: %s:%s has an unknown unit '%s' (want ms, s, m or h)",
		    g_dir, fname, key, val, end);

	if (v > NG_MAX_MS / mul)
		die("%s/%s: %s:%s is longer than the %u ms maximum",
		    g_dir, fname, key, val, NG_MAX_MS);
	v *= mul;
	if (!v)
		die("%s/%s: %s:%s must not be zero", g_dir, fname, key, val);

	return (uint32_t)v;
}

static int has_code(const char *buf)
{
	const char *p = buf;

	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p && *p != '#' && *p != '\n' && *p != '\r')
			return 1;
		p = strchr(p, '\n');
		if (!p)
			break;
		p++;
	}
	return 0;
}

static int heredoc_tag(const char *line, char *tag, size_t cap)
{
	const char *p = strstr(line, "<<");
	size_t n;

	if (!p || p[2] == '<')
		return 0;
	p += 2;
	if (*p == '-')
		p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\'' || *p == '"') {
		char q = *p++;
		const char *e = strchr(p, q);

		if (!e || (size_t)(e - p) >= cap || e == p)
			return 0;
		memcpy(tag, p, (size_t)(e - p));
		tag[e - p] = '\0';
		return 1;
	}
	n = strcspn(p, " \t;|&<>()");
	if (!n || n >= cap)
		return 0;
	memcpy(tag, p, n);
	tag[n] = '\0';
	return 1;
}

// bash line numbers must match the file
static void parse_src(struct src *s, const char *fname, const char *body, size_t len)
{
	char *scratch = xmalloc(len + 1);
	char *line = scratch;
	int code;

	memcpy(scratch, body, len + 1);

	s->name = strdup(fname);
	if (!s->name)
		die("out of memory");
	s->type = NG_TYPE_TARGET;

	while (line && *line) {
		char *nl = strchr(line, '\n');
		char *colon, *key, *val;

		if (nl)
			*nl = '\0';

		key = trim(line);
		if (*key && *key != '#') {
			if (nl)
				*nl = '\n';
			break;
		}
		line = nl ? nl + 1 : NULL;
		if (key[0] != '#' || key[1] != '%')
			continue;
		key = trim(key + 2);
		if (!*key)
			continue;

		colon = strchr(key, ':');
		if (!colon)
			die("%s/%s: directive without a key: #%%%s", g_dir, fname, key);
		*colon = '\0';
		val = trim(colon + 1);
		key = trim(key);

		if (!strcmp(key, "name")) {
			if (strcmp(val, fname))
				die("%s/%s: name:%s does not match its filename; service identity is the filename",
				    g_dir, fname, val);
		} else if (!strcmp(key, "depon")) {
			split_into(&s->depon, val, 1);
		} else if (!strcmp(key, "depof")) {
			split_into(&s->depof, val, 1);
		} else if (!strcmp(key, "onfail")) {
			if (!strcmp(val, "warn"))
				s->onfail = NG_ONFAIL_WARN;
			else if (!strcmp(val, "stop"))
				s->onfail = NG_ONFAIL_STOP;
			else if (!strcmp(val, "shell"))
				s->onfail = NG_ONFAIL_SHELL;
			else
				die("%s/%s: unknown onfail:%s (want warn, stop or shell)",
				    g_dir, fname, val);
			s->have_onfail = 1;
		} else if (!strcmp(key, "restart")) {
			if (!strcmp(val, "always"))
				s->restart = 1;
			else if (!strcmp(val, "no") || !strcmp(val, "never"))
				s->restart = 0;
			else
				die("%s/%s: unknown restart:%s (want always or no)",
				    g_dir, fname, val);
		} else if (!strcmp(key, "notify")) {
			char *end;
			unsigned long fd;

			errno = 0;
			fd = strtoul(val, &end, 10);
			if (*val == '-' || errno || end == val || *end ||
			    fd < NG_NOTIFY_MIN || fd > NG_NOTIFY_MAX)
				die("%s/%s: notify:%s must be a descriptor between %u and %u",
				    g_dir, fname, val, NG_NOTIFY_MIN, NG_NOTIFY_MAX);
			s->notify = (uint16_t)fd;
		} else if (!strcmp(key, "start-timeout")) {
			s->start_ms = parse_ms(val, fname, key);
		} else if (!strcmp(key, "stop-timeout")) {
			s->stop_ms = parse_ms(val, fname, key);
		} else if (!strcmp(key, "start-delay")) {
			uint32_t d = parse_ms(val, fname, key);

			if (d > UINT16_MAX)
				die("%s/%s: start-delay:%s is longer than the %u ms maximum",
				    g_dir, fname, val, UINT16_MAX);
			s->retry_ms = (uint16_t)d;
		} else if (!strcmp(key, "start-tries")) {
			char *end;
			unsigned long t;

			errno = 0;
			t = strtoul(val, &end, 10);
			if (*val == '-' || errno || end == val || *end ||
			    t < 1 || t > NG_MAX_TRIES)
				die("%s/%s: start-tries:%s must be between 1 and %u",
				    g_dir, fname, val, NG_MAX_TRIES);
			s->start_tries = (uint8_t)t;
		} else if (!strcmp(key, "deps")) {
			if (!strcmp(val, "uptime"))
				s->pflags &= (uint8_t)~NG_PF_ORDER_ONLY;
			else if (!strcmp(val, "order"))
				s->pflags |= NG_PF_ORDER_ONLY;
			else
				die("%s/%s: unknown deps:%s (want uptime or order)",
				    g_dir, fname, val);
		} else if (!strcmp(key, "type")) {
			if (!strcmp(val, "oneshot"))
				s->type = NG_TYPE_ONESHOT;
			else if (!strcmp(val, "daemon"))
				s->type = NG_TYPE_DAEMON;
			else if (!strcmp(val, "target"))
				s->type = NG_TYPE_TARGET;
			else
				die("%s/%s: unknown type:%s", g_dir, fname, val);
			s->have_type = 1;
		} else {
			die("%s/%s: unknown directive '%s'", g_dir, fname, key);
		}
	}

	if (strlen(body) != len)
		die("%s/%s: contains a NUL byte", g_dir, fname);

	{
		char tag[128] = "";

		while (line && *line) {
			char *nl = strchr(line, '\n');
			const char *key;

			if (nl)
				*nl++ = '\0';
			key = trim(line);
			line = nl;

			if (*tag) {
				if (!strcmp(key, tag))
					tag[0] = '\0';
				continue;
			}
			if (heredoc_tag(key, tag, sizeof(tag)))
				continue;
			if (key[0] == '#' && key[1] == '%')
				fprintf(stderr, "ninitctl: warning: %s/%s: '%s' comes after the "
					"first command, so it is a plain comment, not a directive\n",
					g_dir, fname, key);
		}
	}

	code = has_code(body);
	if (!s->have_type)
		s->type = code ? NG_TYPE_ONESHOT : NG_TYPE_TARGET;

	if (s->notify && s->type != NG_TYPE_DAEMON)
		die("%s/%s: notify: is only meaningful for type:daemon; a %s %s",
		    g_dir, fname, ng_typename(s->type),
		    s->type == NG_TYPE_TARGET ? "has no process"
					      : "is complete when it exits");

	if (s->restart && s->type != NG_TYPE_DAEMON)
		die("%s/%s: restart: is only meaningful for type:daemon; a %s %s",
		    g_dir, fname, ng_typename(s->type),
		    s->type == NG_TYPE_TARGET ? "has no process"
					      : "is meant to run once and exit");

	if (s->type == NG_TYPE_TARGET) {
		if (code)
			die("%s/%s: type:target must contain no commands", g_dir, fname);
	} else {
		if (!code)
			die("%s/%s: type:%s has no commands to run",
			    g_dir, fname, ng_typename(s->type));
		if (len > NG_MAX_SCRIPT)
			die("%s/%s: script is %zu bytes; execve caps one argument at %u",
			    g_dir, fname, len, NG_MAX_SCRIPT);
		s->script = body;
	}
}

static int keep(const struct dirent *d)
{
	return d->d_name[0] != '.';
}

static int same_dir(const char *a, const char *b)
{
	struct stat sa, sb;

	return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
	       sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int is_artifact(const char *name, const char *base, size_t base_len)
{
	if (!base || strncmp(name, base, base_len))
		return 0;
	return !name[base_len] || !strcmp(name + base_len, ".old") ||
	       !strcmp(name + base_len, ".tmp");
}

static int graph_image(const char *path)
{
	struct ng_hdr h;
	struct stat st;
	ssize_t k;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return 0;
	if (fstat(fd, &st) < 0 || (uint64_t)st.st_size < sizeof(h)) {
		close(fd);
		return 0;
	}
	k = read(fd, &h, sizeof(h));
	close(fd);
	if (k != (ssize_t)sizeof(h))
		return 0;

	return h.magic == NG_MAGIC && h.total_len == (uint32_t)st.st_size;
}

static int cmp_dirent(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

static int cmp_edge(const void *x, const void *y)
{
	const struct edge *a = x, *b = y;

	if (a->a != b->a)
		return a->a < b->a ? -1 : 1;
	if (a->b != b->b)
		return a->b < b->b ? -1 : 1;
	return 0;
}

static int cmp_name_idx(const void *x, const void *y, void *ctx)
{
	const struct src *s = ctx;

	return strcmp(s[*(const uint32_t *)x].name, s[*(const uint32_t *)y].name);
}

// scanning every service per dependency reference is O(edges * services)
static uint32_t lookup(const struct src *s, const uint32_t *byname, uint32_t n,
		       const char *name)
{
	uint32_t lo = 0, hi = n;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		int c = strcmp(s[byname[mid]].name, name);

		if (!c)
			return byname[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return UINT32_MAX;
}

struct graph {
	uint32_t n;
	uint32_t *off;
	uint32_t *idx;
	uint32_t m;
	uint32_t *indeg;
	uint32_t *height;
	uint8_t *color;
	uint32_t *path;
	uint32_t *iter;
	uint32_t depth;
};

static void cycle_death(struct graph *g, struct src *s, uint32_t v)
{
	uint32_t i, start = 0;

	for (i = 0; i < g->depth; i++)
		if (g->path[i] == v) {
			start = i;
			break;
		}

	fputs("ninitctl: dependency cycle:\n  ", stderr);
	for (i = start; i < g->depth; i++)
		fprintf(stderr, "%s -> ", s[g->path[i]].name);
	fprintf(stderr, "%s\n", s[v].name);
	exit(1);
}

static void visit(struct graph *g, struct src *s, uint32_t root)
{
	if (g->color[root])
		return;

	g->color[root] = 1;
	g->path[g->depth] = root;
	g->iter[g->depth] = g->off[root];
	g->depth++;

	while (g->depth) {
		uint32_t top = g->depth - 1;
		uint32_t v = g->path[top];
		uint32_t j, h = 0;

		if (g->iter[top] < g->off[v + 1]) {
			uint32_t w = g->idx[g->iter[top]++];

			if (g->color[w] == 1)
				cycle_death(g, s, w);
			if (g->color[w] == 2)
				continue;

			g->color[w] = 1;
			g->path[g->depth] = w;
			g->iter[g->depth] = g->off[w];
			g->depth++;
			continue;
		}

		// every child is final now, so height is one past the tallest
		for (j = g->off[v]; j < g->off[v + 1]; j++)
			if (g->height[g->idx[j]] + 1 > h)
				h = g->height[g->idx[j]] + 1;
		g->height[v] = h;
		g->color[v] = 2;
		g->depth--;
	}
}

struct ready {
	uint32_t *v;
	uint32_t n;
};

static int ready_before(const struct graph *g, uint32_t a, uint32_t b)
{
	int ra = !g->indeg[a], rb = !g->indeg[b];

	if (ra != rb)
		return ra;
	if (g->height[a] != g->height[b])
		return g->height[a] > g->height[b];
	return a < b;
}

static void ready_push(struct ready *q, const struct graph *g, uint32_t x)
{
	uint32_t i = q->n++;

	q->v[i] = x;
	while (i) {
		uint32_t p = (i - 1) / 2, t;

		if (!ready_before(g, q->v[i], q->v[p]))
			break;
		t = q->v[i];
		q->v[i] = q->v[p];
		q->v[p] = t;
		i = p;
	}
}

static uint32_t ready_pop(struct ready *q, const struct graph *g)
{
	uint32_t top = q->v[0], i = 0;

	q->v[0] = q->v[--q->n];
	for (;;) {
		uint32_t l = 2 * i + 1, r = l + 1, b = i, t;

		if (l < q->n && ready_before(g, q->v[l], q->v[b]))
			b = l;
		if (r < q->n && ready_before(g, q->v[r], q->v[b]))
			b = r;
		if (b == i)
			break;
		t = q->v[i];
		q->v[i] = q->v[b];
		q->v[b] = t;
		i = b;
	}
	return top;
}

static uint32_t *schedule(struct graph *g)
{
	uint32_t *order = xmalloc(g->n * sizeof(*order));
	uint32_t *left = xmalloc(g->n * sizeof(*left));
	struct ready q = { .v = xmalloc(g->n * sizeof(*q.v)), .n = 0 };
	uint32_t done = 0, i;

	memcpy(left, g->indeg, g->n * sizeof(*left));
	for (i = 0; i < g->n; i++)
		if (!left[i])
			ready_push(&q, g, i);

	while (q.n) {
		uint32_t best = ready_pop(&q, g);

		order[done++] = best;
		for (i = g->off[best]; i < g->off[best + 1]; i++)
			if (!--left[g->idx[i]])
				ready_push(&q, g, g->idx[i]);
	}
	if (done != g->n)
		die("internal: no ready node but %u remain", g->n - done);

	free(left);
	free(q.v);
	return order;
}

static void check_syntax(struct src *srcs, uint32_t n, const char *dir)
{
	static char argv0[] = NG_SHELL_ARGV0, dashn[] = "-n";
	static char path[] = NG_PATH;
	static char *const envp[] = { path, NULL };
	static char *const cargv[] = { argv0, dashn, NULL };
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	uint32_t slots = ncpu > 1 ? (uint32_t)ncpu : 1;
	uint32_t i, live = 0, bad = 0;
	pid_t *pids;
	int *errfd;
	uint32_t *who;
	int devnull;

	if (access(NG_SHELL, X_OK) != 0) {
		fprintf(stderr, "ninitctl: warning: %s is not executable here, "
			"skipping the script syntax check\n", NG_SHELL);
		return;
	}
	if (slots > 32)
		slots = 32;
	pids = xmalloc(slots * sizeof(*pids));
	errfd = xmalloc(slots * sizeof(*errfd));
	who = xmalloc(slots * sizeof(*who));
	devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);

	for (i = 0; i <= n; i++) {
		size_t len;
		int sfd, efd;
		pid_t pid;

		while (live == slots || (i == n && live)) {
			int st;
			pid_t got = wait(&st);
			uint32_t k;

			if (got < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			for (k = 0; k < live; k++)
				if (pids[k] == got)
					break;
			if (k == live)
				continue;
			if (!WIFEXITED(st) || WEXITSTATUS(st)) {
				char msg[4096];
				ssize_t mn;

				bad++;
				lseek(errfd[k], 0, SEEK_SET);
				mn = read(errfd[k], msg, sizeof(msg) - 1);
				if (mn > 0) {
					char *q = msg, *nl;

					msg[mn] = '\0';
					// bash saw a memfd so name the service here
					while (q) {
						nl = strchr(q, '\n');
						if (nl)
							*nl++ = '\0';
						if (*q)
							fprintf(stderr, "ninitctl: %s/%s: %s\n",
								dir, srcs[who[k]].name, q);
						q = nl;
					}
				}
			}
			close(errfd[k]);
			live--;
			pids[k] = pids[live];
			errfd[k] = errfd[live];
			who[k] = who[live];
		}
		if (i == n)
			break;
		if (!srcs[i].script)
			continue;

		len = strlen(srcs[i].script);
		sfd = memfd_create(srcs[i].name, 0);
		efd = memfd_create("stderr", 0);
		if (sfd < 0 || efd < 0) {
			if (sfd >= 0)
				close(sfd);
			if (efd >= 0)
				close(efd);
			fprintf(stderr, "ninitctl: warning: memfd_create: %s, "
				"skipping the rest of the syntax check\n", strerror(errno));
			break;
		}
		if ((size_t)write(sfd, srcs[i].script, len) != len ||
		    lseek(sfd, 0, SEEK_SET) != 0) {
			close(sfd);
			close(efd);
			continue;
		}

		pid = fork();
		if (pid < 0)
			die("fork: %s", strerror(errno));
		if (pid == 0) {
			dup2(sfd, 0);
			if (devnull >= 0)
				dup2(devnull, 1);
			dup2(efd, 2);
			execve(NG_SHELL, cargv, envp);
			_exit(127);
		}
		close(sfd);
		pids[live] = pid;
		errfd[live] = efd;
		who[live] = i;
		live++;
	}

	if (devnull >= 0)
		close(devnull);
	free(pids);
	free(errfd);
	free(who);
	if (bad)
		die("%u service script%s did not parse as %s; fix %s and run init again",
		    bad, bad == 1 ? "" : "s", NG_SHELL, bad == 1 ? "it" : "them");
}

struct blob {
	char *p;
	uint32_t n, cap;
};

static uint32_t blob_add(struct blob *b, const char *s)
{
	uint32_t len = (uint32_t)strlen(s) + 1;
	uint32_t at = b->n;

	if (b->n + len > b->cap) {
		b->cap = (b->n + len) * 2 + 64;
		b->p = xrealloc(b->p, b->cap);
	}
	memcpy(b->p + at, s, len);
	b->n += len;
	return at;
}

static int lock_dir(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		die("open %s: %s", path, strerror(errno));
	if (flock(fd, LOCK_EX) < 0)
		die("lock %s: %s", path, strerror(errno));
	return fd;
}

static int same_fd_dir(int fd, const char *path)
{
	struct stat a, b;

	return fstat(fd, &a) == 0 && stat(path, &b) == 0 &&
	       a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

static void write_atomic(const char *path, const void *buf, size_t len, mode_t mode, int held)
{
	char tmp[8224], old[4104], parent[4104], *slash;
	const char *dir, *base;
	const char *p = buf;
	size_t left = len;
	mode_t um;
	int fd, dfd, owned = 0;

	if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent))
		die("output path is too long: %s", path);

	slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		base = path + (slash - parent) + 1;
		dir = *parent ? parent : "/";
	} else {
		base = path;
		dir = ".";
	}

	if (snprintf(tmp, sizeof(tmp), "%s/.%s.XXXXXX", dir, base) >= (int)sizeof(tmp) ||
	    snprintf(old, sizeof(old), "%s.old", path) >= (int)sizeof(old))
		die("output path is too long: %s", path);

	// re-locking a directory this process already holds would deadlock
	if (held >= 0 && same_fd_dir(held, dir)) {
		dfd = held;
	} else {
		dfd = lock_dir(dir);
		owned = 1;
	}

	um = umask(0);
	umask(um);

	fd = mkostemp(tmp, O_CLOEXEC);
	if (fd < 0)
		die("mkstemp %s: %s", tmp, strerror(errno));
	if (fchmod(fd, mode & ~um) < 0) {
		unlink(tmp);
		die("fchmod %s: %s", tmp, strerror(errno));
	}

	while (left) {
		ssize_t n = write(fd, p, left);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			unlink(tmp);
			die("write %s: %s", tmp, strerror(errno));
		}
		p += n;
		left -= (size_t)n;
	}
	if (fsync(fd) < 0) {
		unlink(tmp);
		die("fsync %s: %s", tmp, strerror(errno));
	}
	close(fd);

	unlink(old);
	if (link(path, old) < 0 && errno != ENOENT) {
		unlink(tmp);
		die("link %s -> %s: %s", path, old, strerror(errno));
	}

	if (rename(tmp, path) < 0) {
		unlink(tmp);
		die("rename %s -> %s: %s", tmp, path, strerror(errno));
	}

	if (fsync(dfd) < 0)
		die("%s: published, but syncing %s failed: %s; it may not survive a crash",
		    path, dir, strerror(errno));
	if (owned)
		close(dfd);
}

int cmd_init(int argc, char **argv)
{
	const char *dir = NG_DEFAULT_DIR, *out = NULL;
	char outbuf[4096];
	struct dirent **ents;
	struct src *srcs;
	struct edge *edges = NULL;
	struct graph g = { 0 };
	uint32_t n, m = 0, cap = 0, i, j, nroots = 0;
	uint32_t *order, *inv, *byname;
	uint64_t hash = 0xcbf29ce484222325ull;
	const char *why, *out_base;
	size_t out_base_len;
	mode_t out_mode = 0644;
	int ne, k, custom_dir = 0, dry = 0, nocheck = 0, srclock;

	// argv is already past argv[0] and the subcommand
	for (k = 0; k < argc; k++) {
		if (!strcmp(argv[k], "-d") || !strcmp(argv[k], "--dir")) {
			if (++k == argc)
				usage_die("init: %s needs a directory", argv[k - 1]);
			dir = argv[k];
			custom_dir = 1;
		} else if (!strcmp(argv[k], "-o") || !strcmp(argv[k], "--out")) {
			if (++k == argc)
				usage_die("init: %s needs a file", argv[k - 1]);
			out = argv[k];
		} else if (!strcmp(argv[k], "-n") || !strcmp(argv[k], "--dry-run")) {
			dry = 1;
		} else if (!strcmp(argv[k], "--no-check")) {
			nocheck = 1;
		} else {
			usage_die("init: unexpected argument '%s'", argv[k]);
		}
	}
	g_dir = dir;
	if (!out) {
		if (!custom_dir) {
			out = NG_DEFAULT_FILE;
		} else {
			if (snprintf(outbuf, sizeof(outbuf), "%s/depgraph", dir) >= (int)sizeof(outbuf))
				usage_die("init: directory path is too long: %s", dir);
			out = outbuf;
		}
	}

	{
		char outdir[4096];
		char *slash;

		if (snprintf(outdir, sizeof(outdir), "%s", out) >= (int)sizeof(outdir))
			usage_die("init: output path is too long: %s", out);
		slash = strrchr(outdir, '/');
		if (slash) {
			out_base = out + (slash - outdir) + 1;
			if (slash == outdir)
				outdir[1] = '\0';
			else
				*slash = '\0';
		} else {
			out_base = out;
			outdir[0] = '.';
			outdir[1] = '\0';
		}
		if (!*out_base || !same_dir(dir, outdir))
			out_base = NULL;
		out_base_len = out_base ? strlen(out_base) : 0;
	}

	srclock = lock_dir(dir);

	ne = scandir(dir, &ents, keep, cmp_dirent);
	if (ne < 0)
		die("scandir %s: %s", dir, strerror(errno));
	if ((uint32_t)ne > NG_MAX_SVC + 4)
		die("%d entries in %s; the tested maximum is %u services", ne, dir, NG_MAX_SVC);

	srcs = xmalloc((size_t)ne * sizeof(*srcs));
	n = 0;
	for (k = 0; k < ne; k++) {
		char path[4096];
		struct stat st;
		char *buf;
		size_t len;
		mode_t sm;

		int is_out = is_artifact(ents[k]->d_name, out_base, out_base_len);
		int is_dg = is_artifact(ents[k]->d_name, "depgraph", 8) ||
			    ng_reserved_name(ents[k]->d_name);

		if (snprintf(path, sizeof(path), "%s/%s", dir, ents[k]->d_name) >= (int)sizeof(path))
			die("%s/%s: path is too long", dir, ents[k]->d_name);
		if (stat(path, &st) < 0)
			die("stat %s: %s", path, strerror(errno));

		if (is_out || is_dg) {
			if (!S_ISREG(st.st_mode) || !graph_image(path)) {
				if (is_out)
					die("%s/%s: the output would replace this, and it is not "
					    "a depgraph; pick another -o name",
					    dir, ents[k]->d_name);
				fprintf(stderr, "ninitctl: warning: %s/%s has a name ninitctl "
					"reserves and was not built; rename it if it is a service\n",
					dir, ents[k]->d_name);
			}
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;

		why = ng_name_problem(ents[k]->d_name);
		if (why)
			die("%s/%s: filename %s", dir, ents[k]->d_name, why);

		sm = st.st_mode;
		if (!(sm & S_IRGRP))
			out_mode &= (mode_t)~(S_IRGRP | S_IWGRP);
		if (!(sm & S_IROTH))
			out_mode &= (mode_t)~(S_IROTH | S_IWOTH);

		if ((uint64_t)st.st_size > NG_MAX_SRC && graph_image(path)) {
			fprintf(stderr, "ninitctl: warning: %s is a compiled depgraph, "
				"not a service; skipping it\n", path);
			continue;
		}

		buf = slurp(path, &len);

		// a graph left in the services directory under any other name
		if (len >= sizeof(struct ng_hdr) &&
		    ((const struct ng_hdr *)(const void *)buf)->magic == NG_MAGIC &&
		    ((const struct ng_hdr *)(const void *)buf)->total_len == len) {
			fprintf(stderr, "ninitctl: warning: %s is a compiled depgraph, "
				"not a service; skipping it\n", path);
			free(buf);
			continue;
		}

		for (const char *p = ents[k]->d_name; *p; p++)
			hash = (hash ^ (unsigned char)*p) * 0x100000001b3ull;
		hash = (hash ^ 0) * 0x100000001b3ull;
		for (size_t q = 0; q < len; q++)
			hash = (hash ^ (unsigned char)buf[q]) * 0x100000001b3ull;
		hash = (hash ^ 0) * 0x100000001b3ull;

		parse_src(&srcs[n++], ents[k]->d_name, buf, len);
	}
	if (!n)
		die("no services in %s", dir);
	if (n > NG_MAX_SVC)
		die("%u services in %s; the tested maximum is %u", n, dir, NG_MAX_SVC);

	if (!nocheck)
		check_syntax(srcs, n, dir);

	byname = xmalloc(n * sizeof(*byname));
	for (i = 0; i < n; i++)
		byname[i] = i;
	qsort_r(byname, n, sizeof(*byname), cmp_name_idx, srcs);

	for (i = 0; i < n; i++) {
		for (j = 0; j < srcs[i].depon.n + srcs[i].depof.n; j++) {
			int rev = j >= srcs[i].depon.n;
			const char *name = rev ? srcs[i].depof.v[j - srcs[i].depon.n]
					       : srcs[i].depon.v[j];
			uint32_t o = lookup(srcs, byname, n, name);

			if (o == UINT32_MAX)
				die("%s/%s: %s:%s names no service in %s",
				    dir, srcs[i].name, rev ? "depof" : "depon", name, dir);
			if (o == i)
				die("%s/%s: depends on itself", dir, srcs[i].name);

			if (m == cap) {
				cap = cap ? cap * 2 : 32;
				edges = xrealloc(edges, cap * sizeof(*edges));
			}
			edges[m].a = rev ? i : o;
			edges[m].b = rev ? o : i;
			m++;
		}
	}

	if (m)
		qsort(edges, m, sizeof(*edges), cmp_edge);
	j = 0;
	for (i = 0; i < m; i++)
		if (!i || cmp_edge(&edges[i], &edges[j - 1]))
			edges[j++] = edges[i];
	m = j;

	g.n = n;
	g.m = m;
	g.off = xmalloc((n + 1) * sizeof(*g.off));
	g.idx = xmalloc((m ? m : 1) * sizeof(*g.idx));
	g.indeg = xmalloc(n * sizeof(*g.indeg));
	g.height = xmalloc(n * sizeof(*g.height));
	g.color = xmalloc(n);
	g.path = xmalloc(n * sizeof(*g.path));
	g.iter = xmalloc(n * sizeof(*g.iter));

	for (i = 0; i < m; i++) {
		g.off[edges[i].a + 1]++;
		g.indeg[edges[i].b]++;
	}
	for (i = 0; i < n; i++)
		g.off[i + 1] += g.off[i];
	{
		uint32_t *fill = xmalloc(n * sizeof(*fill));

		for (i = 0; i < m; i++)
			g.idx[g.off[edges[i].a] + fill[edges[i].a]++] = edges[i].b;
		free(fill);
	}

	for (i = 0; i < n; i++)
		visit(&g, srcs, i);

	order = schedule(&g);
	inv = xmalloc(n * sizeof(*inv));
	for (i = 0; i < n; i++)
		inv[order[i]] = i;
	for (i = 0; i < n; i++)
		nroots += !g.indeg[i];

	for (i = 0; i < m; i++) {
		edges[i].a = inv[edges[i].a];
		edges[i].b = inv[edges[i].b];
	}
	if (m)
		qsort(edges, m, sizeof(*edges), cmp_edge);

	{
		struct blob blob = { 0 };
		uint32_t *roff = xmalloc((n + 1) * sizeof(*roff));
		uint32_t *ridx = xmalloc((m ? m : 1) * sizeof(*ridx));
		uint32_t nw, vague = 0;
		uint64_t *desc;
		struct ng_svc *sv = xmalloc(n * sizeof(*sv));
		struct ng_pol *pl = xmalloc(n * sizeof(*pl));
		struct ng_hdr *h;
		char *buf;
		size_t off, total;

		for (i = 0; i < m; i++)
			roff[edges[i].a + 1]++;
		for (i = 0; i < n; i++)
			roff[i + 1] += roff[i];
		for (i = 0; i < m; i++)
			ridx[i] = edges[i].b;

		nw = (n + 63) / 64;
		desc = xmalloc((size_t)n * nw * sizeof(*desc));
		for (i = n; i-- > 0;) {
			uint64_t *d = desc + (size_t)i * nw;

			for (j = roff[i]; j < roff[i + 1]; j++) {
				const uint64_t *sub = desc + (size_t)ridx[j] * nw;
				uint32_t w;

				d[ridx[j] >> 6] |= 1ull << (ridx[j] & 63);
				for (w = 0; w < nw; w++)
					d[w] |= sub[w];
			}
		}

		for (i = 0; i < n; i++) {
			struct src *s = &srcs[order[i]];

			uint32_t nd = 0, w;
			uint8_t pol;


			for (w = 0; w < nw; w++)
				nd += (uint32_t)__builtin_popcountll(desc[(size_t)i * nw + w]);

			if (s->have_onfail) {
				if (s->onfail == NG_ONFAIL_WARN && nd)
					die("%s/%s: onfail:warn but %u service%s depend%s on it",
					    g_dir, s->name, nd, nd == 1 ? "" : "s",
					    nd == 1 ? "s" : "");
				pol = s->onfail;
			} else {
				// only this service decides; adding unrelated ones must not move it
				pol = nd ? NG_ONFAIL_STOP : NG_ONFAIL_WARN;
			}

			if (s->type == NG_TYPE_DAEMON && !s->notify && roff[i] != roff[i + 1])
				vague++;

			sv[i].unmet = (uint16_t)g.indeg[order[i]];
			sv[i].type = s->type;
			sv[i].flags = pol | (s->restart ? NG_FLAG_RESTART : 0);
			sv[i].n_desc = (uint16_t)nd;
			sv[i].notify_fd = s->notify;
			sv[i].name_off = blob_add(&blob, s->name);
			sv[i].script_off = s->script ? blob_add(&blob, s->script)
						     : NG_NO_SCRIPT;

			pl[i].start_ms = s->start_ms;
			pl[i].stop_ms = s->stop_ms;
			pl[i].retry_ms = s->retry_ms;
			pl[i].start_tries = s->start_tries;
			pl[i].pflags = s->pflags;
		}

		off = sizeof(struct ng_hdr);
		total = off;
		total += (size_t)n * sizeof(struct ng_svc);
		total += ((size_t)n + 1) * 4;
		total += (size_t)m * 4;
		total += (size_t)n * sizeof(struct ng_pol);
		total += blob.n;

		buf = xmalloc(total);
		h = (void *)buf;
		h->magic = NG_MAGIC;
		h->version = NG_VERSION;
		h->total_len = (uint32_t)total;
		h->n_svc = n;
		h->n_roots = nroots;
		h->n_edges = m;
		h->blob_len = blob.n;
		h->srcs_hash = hash;

		h->off_svc = (uint32_t)off;
		memcpy(buf + off, sv, (size_t)n * sizeof(*sv));
		off += (size_t)n * sizeof(*sv);

		h->off_rdep_off = (uint32_t)off;
		memcpy(buf + off, roff, ((size_t)n + 1) * 4);
		off += ((size_t)n + 1) * 4;

		h->off_rdep_idx = (uint32_t)off;
		memcpy(buf + off, ridx, (size_t)m * 4);
		off += (size_t)m * 4;

		h->off_pol = (uint32_t)off;
		memcpy(buf + off, pl, (size_t)n * sizeof(*pl));
		off += (size_t)n * sizeof(*pl);

		h->off_blob = (uint32_t)off;
		memcpy(buf + off, blob.p, blob.n);

		h->crc32 = ng_image_crc32c(buf, total);

		why = ng_verify(buf, total);
		if (why)
			die("internal: built a graph that fails verify: %s", why);

		if (dry) {
			printf("%s: would write %u services, %u edges, %u roots, %zu bytes, "
			       "mode %04o\n", out, n, m, nroots, total, (unsigned)out_mode);
		} else {
			write_atomic(out, buf, total, out_mode, srclock);
			printf("%s: %u services, %u edges, %u roots, %zu bytes\n",
			       out, n, m, nroots, total);
		}

		if (vague) {
			fflush(stdout);
			fprintf(stderr,
				"ninitctl: warning: %u daemon%s with dependents %s no notify:, so those\n"
				"ninitctl: dependents start once the shell execs, not once the daemon is ready\n",
				vague, vague == 1 ? "" : "s", vague == 1 ? "has" : "have");
			for (i = 0; i < n; i++) {
				const struct src *s = &srcs[order[i]];

				if (s->type == NG_TYPE_DAEMON && !s->notify &&
				    roff[i] != roff[i + 1])
					fprintf(stderr, "ninitctl:   %s\n", s->name);
			}
		}
	}

	{
		char loc[NG_LOCALE_MAX][NG_LOCALE_LEN];
		const char *bad;
		int nl = ng_locale_env(loc, NG_LOCALE_MAX, &bad);

		fflush(stdout);
		if (bad)
			fprintf(stderr, "ninitctl: warning: %s %s\n", NG_LOCALE_CONF, bad);
		if (nl <= 0)
			fprintf(stderr,
				"ninitctl: %s %s, so services will run with LANG=%s\n"
				"ninitctl: create it, e.g. printf 'LANG=en_US.UTF-8\\n' > %s\n",
				NG_LOCALE_CONF,
				access(NG_LOCALE_CONF, R_OK) ? "is missing"
							     : "sets no locale variable",
				NG_FALLBACK_LANG, NG_LOCALE_CONF);
	}

	return 0;
}
