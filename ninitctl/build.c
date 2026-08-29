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
#include <sys/stat.h>

struct strv {
	char **v;
	uint32_t n, cap;
};

struct src {
	char *name;
	char **argv;
	uint32_t argc;
	uint8_t type;
	int have_type;
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

static void parse_src(struct src *s, const char *fname, char *buf)
{
	struct strv args = { 0 };
	char *line = buf;

	s->name = strdup(fname);
	s->type = NG_TYPE_TARGET;

	while (line && *line) {
		char *nl = strchr(line, '\n');
		char *colon, *key, *val;

		if (nl)
			*nl++ = '\0';

		key = trim(line);
		line = nl;

		if (!*key || *key == '#')
			continue;

		colon = strchr(key, ':');
		if (!colon)
			die("%s/%s: line without a key: %s", g_dir, fname, key);
		*colon = '\0';
		val = trim(colon + 1);
		key = trim(key);

		if (!strcmp(key, "name")) {
			if (strcmp(val, fname))
				die("%s/%s: name:%s does not match its filename; service identity is the filename",
				    g_dir, fname, val);
		} else if (!strcmp(key, "exe")) {
			if (args.n)
				die("%s/%s: duplicate exe:", g_dir, fname);
			split_into(&args, val, 0);
			if (!args.n)
				die("%s/%s: empty exe:", g_dir, fname);
		} else if (!strcmp(key, "depon")) {
			split_into(&s->depon, val, 1);
		} else if (!strcmp(key, "depof")) {
			split_into(&s->depof, val, 1);
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
			die("%s/%s: unknown key '%s'", g_dir, fname, key);
		}
	}

	s->argv = args.v;
	s->argc = args.n;

	if (!s->have_type)
		s->type = s->argc ? NG_TYPE_DAEMON : NG_TYPE_TARGET;
	if (s->type == NG_TYPE_TARGET && s->argc)
		die("%s/%s: type:target cannot have exe:", g_dir, fname);
	if (s->type != NG_TYPE_TARGET && !s->argc)
		die("%s/%s: type:%s needs an exe:", g_dir, fname, ng_typename(s->type));
}

static int keep(const struct dirent *d)
{
	return d->d_name[0] != '.';
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

static uint32_t lookup(struct src *s, uint32_t n, const char *name)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (!strcmp(s[i].name, name))
			return i;
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

static void visit(struct graph *g, struct src *s, uint32_t v)
{
	uint32_t j, h = 0;

	if (g->color[v] == 1)
		cycle_death(g, s, v);
	if (g->color[v] == 2)
		return;

	g->color[v] = 1;
	g->path[g->depth++] = v;

	for (j = g->off[v]; j < g->off[v + 1]; j++) {
		uint32_t w = g->idx[j];

		visit(g, s, w);
		if (g->height[w] + 1 > h)
			h = g->height[w] + 1;
	}

	g->depth--;
	g->color[v] = 2;
	g->height[v] = h;
}

static uint32_t *schedule(struct graph *g)
{
	uint32_t *order = xmalloc(g->n * sizeof(*order));
	uint32_t *left = xmalloc(g->n * sizeof(*left));
	uint32_t done = 0, i;

	memcpy(left, g->indeg, g->n * sizeof(*left));

	while (done < g->n) {
		uint32_t best = UINT32_MAX;

		for (i = 0; i < g->n; i++) {
			int ri, rb;

			if (left[i] != 0)
				continue;
			if (best == UINT32_MAX) {
				best = i;
				continue;
			}
			ri = !g->indeg[i];
			rb = !g->indeg[best];
			if (ri != rb) {
				if (ri)
					best = i;
			} else if (g->height[i] > g->height[best]) {
				best = i;
			}
		}
		if (best == UINT32_MAX)
			die("internal: no ready node but %u remain", g->n - done);

		order[done++] = best;
		left[best] = UINT32_MAX;
		for (i = g->off[best]; i < g->off[best + 1]; i++)
			left[g->idx[i]]--;
	}

	free(left);
	return order;
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

static void write_atomic(const char *path, const void *buf, size_t len)
{
	char tmp[4104], old[4104], *slash;
	int fd;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	snprintf(old, sizeof(old), "%s.old", path);

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		die("open %s: %s", tmp, strerror(errno));
	if (write(fd, buf, len) != (ssize_t)len)
		die("write %s: %s", tmp, strerror(errno));
	if (fsync(fd) < 0)
		die("fsync %s: %s", tmp, strerror(errno));
	close(fd);

	// a bad build shouldnt be catastrophic
	if (rename(path, old) < 0 && errno != ENOENT)
		die("rename %s -> %s: %s", path, old, strerror(errno));
	if (rename(tmp, path) < 0)
		die("rename %s -> %s: %s", tmp, path, strerror(errno));

	snprintf(tmp, sizeof(tmp), "%s", path);
	slash = strrchr(tmp, '/');
	if (slash) {
		*slash = '\0';
		fd = open(*tmp ? tmp : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (fd >= 0) {
			fsync(fd);
			close(fd);
		}
	}
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
	uint32_t *order, *inv;
	uint64_t hash = 0xcbf29ce484222325ull;
	int ne, k;

	for (k = 0; k < argc; k++) {
		if ((!strcmp(argv[k], "-d") || !strcmp(argv[k], "--dir")) && k + 1 < argc)
			dir = argv[++k];
		else if ((!strcmp(argv[k], "-o") || !strcmp(argv[k], "--out")) && k + 1 < argc)
			out = argv[++k];
		else
			die("init: unexpected argument '%s'", argv[k]);
	}
	g_dir = dir;
	if (!out) {
		snprintf(outbuf, sizeof(outbuf), "%s/depgraph", dir);
		out = outbuf;
	}

	ne = scandir(dir, &ents, keep, alphasort);
	if (ne < 0)
		die("scandir %s: %s", dir, strerror(errno));

	srcs = xmalloc((size_t)ne * sizeof(*srcs));
	n = 0;
	for (k = 0; k < ne; k++) {
		char path[4096];
		struct stat st;
		char *buf;
		size_t len;

		snprintf(path, sizeof(path), "%s/%s", dir, ents[k]->d_name);
		if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
			continue;
		if (!strcmp(ents[k]->d_name, "depgraph") ||
		    !strncmp(ents[k]->d_name, "depgraph.", 9))
			continue;

		buf = slurp(path, &len);

		for (const char *p = ents[k]->d_name; *p; p++)
			hash = (hash ^ (unsigned char)*p) * 0x100000001b3ull;
		for (size_t q = 0; q < len; q++)
			hash = (hash ^ (unsigned char)buf[q]) * 0x100000001b3ull;

		parse_src(&srcs[n++], ents[k]->d_name, buf);
	}
	if (!n)
		die("no services in %s", dir);

	for (i = 0; i < n; i++) {
		for (j = 0; j < srcs[i].depon.n + srcs[i].depof.n; j++) {
			int rev = j >= srcs[i].depon.n;
			const char *name = rev ? srcs[i].depof.v[j - srcs[i].depon.n]
					       : srcs[i].depon.v[j];
			uint32_t o = lookup(srcs, n, name);

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
	qsort(edges, m, sizeof(*edges), cmp_edge);

	{
		struct blob blob = { 0 };
		uint32_t *roff = xmalloc((n + 1) * sizeof(*roff));
		uint32_t *ridx = xmalloc((m ? m : 1) * sizeof(*ridx));
		uint32_t *argvt, nargv = 0, ai = 0;
		struct ng_svc *sv = xmalloc(n * sizeof(*sv));
		struct ng_hdr *h;
		char *buf;
		size_t off, total;

		for (i = 0; i < m; i++)
			roff[edges[i].a + 1]++;
		for (i = 0; i < n; i++)
			roff[i + 1] += roff[i];
		for (i = 0; i < m; i++)
			ridx[i] = edges[i].b;

		for (i = 0; i < n; i++)
			nargv += srcs[i].argc;
		argvt = xmalloc((nargv ? nargv : 1) * sizeof(*argvt));

		for (i = 0; i < n; i++) {
			struct src *s = &srcs[order[i]];

			sv[i].unmet = (uint16_t)g.indeg[order[i]];
			sv[i].type = s->type;
			sv[i].flags = 0;
			sv[i].argc = (uint16_t)s->argc;
			sv[i].pad = 0;
			sv[i].argv_off = ai;
			sv[i].name_off = blob_add(&blob, s->name);
			for (j = 0; j < s->argc; j++)
				argvt[ai++] = blob_add(&blob, s->argv[j]);
		}

		off = sizeof(struct ng_hdr);
		total = off;
		total += (size_t)n * sizeof(struct ng_svc);
		total += ((size_t)n + 1) * 4;
		total += (size_t)m * 4;
		total += (size_t)nargv * 4;
		total += blob.n;

		buf = xmalloc(total);
		h = (struct ng_hdr *)buf;
		h->magic = NG_MAGIC;
		h->version = NG_VERSION;
		h->total_len = (uint32_t)total;
		h->n_svc = n;
		h->n_roots = nroots;
		h->n_edges = m;
		h->blob_len = blob.n;
		h->n_argv = nargv;
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

		h->off_argv = (uint32_t)off;
		memcpy(buf + off, argvt, (size_t)nargv * 4);
		off += (size_t)nargv * 4;

		h->off_blob = (uint32_t)off;
		memcpy(buf + off, blob.p, blob.n);

		h->crc32 = ng_crc32c(buf + sizeof(*h), total - sizeof(*h));

		{
			const char *why = ng_verify(buf, total);

			if (why)
				die("internal: built a graph that fails verify: %s", why);
		}

		write_atomic(out, buf, total);
		printf("%s: %u services, %u edges, %u roots, %zu bytes\n",
		       out, n, m, nroots, total);
	}

	return 0;
}
