#include "init.h"
#include "util.h"
#include "parser/parser.h"
#include "../src/ngraph.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_edge(const void *x, const void *y)
{
	const struct edge *a = x, *b = y;

	if (a->a != b->a)
		return a->a < b->a ? -1 : 1;
	if (a->b != b->b)
		return a->b < b->b ? -1 : 1;
	return 0;
}

static const struct src *cmp_srcs;

static int cmp_name_idx(const void *x, const void *y)
{
	return strcmp(cmp_srcs[*(const uint32_t *)x].name,
		      cmp_srcs[*(const uint32_t *)y].name);
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
		die("internal error: no ready node with %u remaining", g->n - done);

	free(left);
	free(q.v);
	return order;
}

void link_graph(struct build *b)
{
	const char *dir = b->dir;
	struct src *srcs = b->srcs;
	struct edge *edges = NULL;
	struct graph g = { 0 };
	uint32_t n = b->n, m = 0, cap = 0, i, j;
	uint32_t *byname;

	byname = xmalloc(n * sizeof(*byname));
	for (i = 0; i < n; i++)
		byname[i] = i;
	cmp_srcs = srcs;
	qsort(byname, n, sizeof(*byname), cmp_name_idx);

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

	b->edges = edges;
	b->m = m;
	b->g = g;
}

void order_graph(struct build *b)
{
	struct graph g = b->g;
	struct edge *edges = b->edges;
	uint32_t n = b->n, m = b->m, i, nroots = 0;
	uint32_t *order, *inv;

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

	b->order = order;
	b->nroots = nroots;
}
