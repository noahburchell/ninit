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
	const char *script;
	uint8_t type;
	int have_type;
	uint8_t onfail;
	int have_onfail;
	uint8_t restart;
	uint16_t notify;
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
			*nl++ = '\0';

		key = trim(line);
		line = nl;

		if (*key && *key != '#')
			break;
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

	while (line && *line) {
		char *nl = strchr(line, '\n');
		const char *key;

		if (nl)
			*nl++ = '\0';
		key = trim(line);
		line = nl;
		if (key[0] == '#' && key[1] == '%')
			die("%s/%s: directive '%s' comes after the first command; directives must lead the file",
			    g_dir, fname, key);
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

static int cmp_dirent(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

static const char *name_problem(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t n = strlen(s);

	if (n && s[n - 1] == '~')
		return "looks like an editor backup; delete it or move it to unused/";
	if (n > 1 && s[0] == '#' && s[n - 1] == '#')
		return "looks like an editor autosave; delete it or move it to unused/";
	for (; *p; p++) {
		if (*p == ',')
			return "contains a comma; depon/depof could never name it";
		if (*p <= ' ' || *p == 0x7f)
			return "contains whitespace or a control character; depon/depof could never name it";
	}
	return NULL;
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
	const char *dir;
	const char *p = buf;
	size_t left = len;
	int fd;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp) ||
	    snprintf(old, sizeof(old), "%s.old", path) >= (int)sizeof(old))
		die("output path is too long: %s", path);

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		die("open %s: %s", tmp, strerror(errno));

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
	if (link(path, old) < 0 && errno != ENOENT)
		die("link %s -> %s: %s", path, old, strerror(errno));

	if (rename(tmp, path) < 0) {
		unlink(tmp);
		die("rename %s -> %s: %s", tmp, path, strerror(errno));
	}

	snprintf(tmp, sizeof(tmp), "%s", path);
	slash = strrchr(tmp, '/');
	if (slash)
		*slash = '\0';
	dir = !slash ? "." : *tmp ? tmp : "/";

	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
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
	const char *why;
	int ne, k, custom_dir = 0;

	// argv is already past argv[0] and the subcommand
	for (k = 0; k < argc; k++) {
		if (!strcmp(argv[k], "-d") || !strcmp(argv[k], "--dir")) {
			if (++k == argc)
				die("init: %s needs a directory", argv[k - 1]);
			dir = argv[k];
			custom_dir = 1;
		} else if (!strcmp(argv[k], "-o") || !strcmp(argv[k], "--out")) {
			if (++k == argc)
				die("init: %s needs a file", argv[k - 1]);
			out = argv[k];
		} else {
			die("init: unexpected argument '%s'", argv[k]);
		}
	}
	g_dir = dir;
	if (!out) {
		if (!custom_dir) {
			out = NG_DEFAULT_FILE;
		} else {
			if (snprintf(outbuf, sizeof(outbuf), "%s/depgraph", dir) >= (int)sizeof(outbuf))
				die("init: directory path is too long: %s", dir);
			out = outbuf;
		}
	}

	ne = scandir(dir, &ents, keep, cmp_dirent);
	if (ne < 0)
		die("scandir %s: %s", dir, strerror(errno));

	srcs = xmalloc((size_t)ne * sizeof(*srcs));
	n = 0;
	for (k = 0; k < ne; k++) {
		char path[4096];
		struct stat st;
		char *buf;
		size_t len;

		if (snprintf(path, sizeof(path), "%s/%s", dir, ents[k]->d_name) >= (int)sizeof(path))
			die("%s/%s: path is too long", dir, ents[k]->d_name);
		if (stat(path, &st) < 0)
			die("stat %s: %s", path, strerror(errno));
		if (!S_ISREG(st.st_mode))
			continue;

		if (!strcmp(ents[k]->d_name, "depgraph") ||
		    !strncmp(ents[k]->d_name, "depgraph.", 9))
			continue;

		why = name_problem(ents[k]->d_name);
		if (why)
			die("%s/%s: filename %s", dir, ents[k]->d_name, why);

		buf = slurp(path, &len);

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
		uint32_t nw;
		uint64_t *desc;
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
			} else if (!nd)
				pol = NG_ONFAIL_WARN;
			else if (nd >= 2 && nd * 2 >= n)
				pol = NG_ONFAIL_SHELL;
			else
				pol = NG_ONFAIL_STOP;

			sv[i].unmet = (uint16_t)g.indeg[order[i]];
			sv[i].type = s->type;
			sv[i].flags = pol | (s->restart ? NG_FLAG_RESTART : 0);
			sv[i].n_desc = (uint16_t)nd;
			sv[i].notify_fd = s->notify;
			sv[i].name_off = blob_add(&blob, s->name);
			sv[i].script_off = s->script ? blob_add(&blob, s->script)
						     : NG_NO_SCRIPT;
		}

		off = sizeof(struct ng_hdr);
		total = off;
		total += (size_t)n * sizeof(struct ng_svc);
		total += ((size_t)n + 1) * 4;
		total += (size_t)m * 4;
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

		h->off_blob = (uint32_t)off;
		memcpy(buf + off, blob.p, blob.n);

		h->crc32 = ng_image_crc32c(buf, total);

		why = ng_verify(buf, total);
		if (why)
			die("internal: built a graph that fails verify: %s", why);

		write_atomic(out, buf, total);
		printf("%s: %u services, %u edges, %u roots, %zu bytes\n",
		       out, n, m, nroots, total);
	}

	{
		char lang[96];

		fflush(stdout);
		if (!ng_locale_lang(lang, sizeof(lang)))
			fprintf(stderr,
				"ninitctl: %s %s, so services will run with LANG=%s\n"
				"ninitctl: create it, e.g. printf 'LANG=en_US.UTF-8\\n' > %s\n",
				NG_LOCALE_CONF,
				access(NG_LOCALE_CONF, R_OK) ? "is missing" : "sets no LANG",
				NG_FALLBACK_LANG, NG_LOCALE_CONF);
	}

	return 0;
}
