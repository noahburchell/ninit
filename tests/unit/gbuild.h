#pragma once

// builds depgraph images independently of ninitctl so the loader is tested against
// the format as documented, not against whatever the compiler happens to emit

#include "ngraph.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GB_DEFAULT	(-1)

struct gb_svc {
	const char *name;
	uint8_t type;
	int onfail;
	uint8_t restart;
	uint16_t notify;
	const char *script;
	struct ng_pol pol;
};

struct gb_edge {
	uint32_t a, b;
};

struct gb_img {
	void *map;
	size_t len;
};

static inline int gb_cmp_u32(const void *x, const void *y)
{
	uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;

	return a < b ? -1 : a > b;
}

static inline size_t gb_align8(size_t v)
{
	return (v + 7) & ~(size_t)7;
}

// edges must run from a lower to a higher index, roots must come first
static inline struct gb_img gb_build(const struct gb_svc *sv, uint32_t n,
				     const struct gb_edge *e, uint32_t m)
{
	struct gb_img img = { 0 };
	uint32_t *roff = calloc((size_t)n + 1, sizeof(*roff));
	uint32_t *ridx = calloc(m ? m : 1, sizeof(*ridx));
	uint32_t *indeg = calloc(n ? n : 1, sizeof(*indeg));
	uint32_t *fill = calloc(n ? n : 1, sizeof(*fill));
	uint8_t *reach = calloc((size_t)n * n + 1, 1);
	size_t blob_len = 0, off, total;
	struct ng_hdr *h;
	char *buf, *blob;
	uint32_t i, j, roots = 0;

	for (i = 0; i < n; i++) {
		blob_len += strlen(sv[i].name) + 1;
		if (sv[i].type != NG_TYPE_TARGET)
			blob_len += strlen(sv[i].script ? sv[i].script : "") + 1;
	}
	for (i = 0; i < m; i++) {
		roff[e[i].a + 1]++;
		indeg[e[i].b]++;
	}
	for (i = 0; i < n; i++)
		roff[i + 1] += roff[i];
	for (i = 0; i < m; i++)
		ridx[roff[e[i].a] + fill[e[i].a]++] = e[i].b;
	for (i = 0; i < n; i++)
		qsort(ridx + roff[i], roff[i + 1] - roff[i], sizeof(*ridx), gb_cmp_u32);
	for (i = n; i-- > 0;)
		for (j = roff[i]; j < roff[i + 1]; j++) {
			reach[(size_t)i * n + ridx[j]] = 1;
			for (uint32_t k = 0; k < n; k++)
				reach[(size_t)i * n + k] |= reach[(size_t)ridx[j] * n + k];
		}
	for (i = 0; i < n; i++)
		roots += !indeg[i];

	total = sizeof(struct ng_hdr) + (size_t)n * sizeof(struct ng_svc) +
		((size_t)n + 1) * 4 + (size_t)m * 4 + (size_t)n * sizeof(struct ng_pol) + blob_len;
	buf = aligned_alloc(8, gb_align8(total) + 8);
	memset(buf, 0, gb_align8(total) + 8);
	h = (struct ng_hdr *)(void *)buf;
	h->magic = NG_MAGIC;
	h->version = NG_VERSION;
	h->total_len = (uint32_t)total;
	h->n_svc = n;
	h->n_roots = roots;
	h->n_edges = m;
	h->blob_len = (uint32_t)blob_len;

	off = sizeof(*h);
	h->off_svc = (uint32_t)off;
	off += (size_t)n * sizeof(struct ng_svc);
	h->off_rdep_off = (uint32_t)off;
	memcpy(buf + off, roff, ((size_t)n + 1) * 4);
	off += ((size_t)n + 1) * 4;
	h->off_rdep_idx = (uint32_t)off;
	memcpy(buf + off, ridx, (size_t)m * 4);
	off += (size_t)m * 4;
	h->off_pol = (uint32_t)off;
	off += (size_t)n * sizeof(struct ng_pol);
	h->off_blob = (uint32_t)off;
	blob = buf + off;

	off = 0;
	for (i = 0; i < n; i++) {
		struct ng_svc *s = (struct ng_svc *)(void *)(buf + h->off_svc) + i;
		struct ng_pol *p = (struct ng_pol *)(void *)(buf + h->off_pol) + i;
		uint32_t nd = 0;
		int onfail = sv[i].onfail;

		for (j = 0; j < n; j++)
			nd += reach[(size_t)i * n + j];
		if (onfail == GB_DEFAULT)
			onfail = nd ? NG_ONFAIL_STOP : NG_ONFAIL_WARN;

		s->unmet = (uint16_t)indeg[i];
		s->type = sv[i].type;
		s->flags = (uint8_t)onfail | (sv[i].restart ? NG_FLAG_RESTART : 0);
		s->n_desc = (uint16_t)nd;
		s->notify_fd = sv[i].notify;
		s->name_off = (uint32_t)off;
		memcpy(blob + off, sv[i].name, strlen(sv[i].name) + 1);
		off += strlen(sv[i].name) + 1;
		if (sv[i].type == NG_TYPE_TARGET) {
			s->script_off = NG_NO_SCRIPT;
		} else {
			const char *sc = sv[i].script ? sv[i].script : "";

			s->script_off = (uint32_t)off;
			memcpy(blob + off, sc, strlen(sc) + 1);
			off += strlen(sc) + 1;
		}
		*p = sv[i].pol;
	}
	h->crc32 = ng_image_crc32c(buf, total);

	free(roff);
	free(ridx);
	free(indeg);
	free(fill);
	free(reach);
	img.map = buf;
	img.len = total;
	return img;
}

static inline void gb_recrc(struct gb_img *g)
{
	struct ng_hdr *h = g->map;

	h->crc32 = ng_image_crc32c(g->map, g->len);
}

static inline struct gb_img gb_copy(const struct gb_img *g)
{
	struct gb_img c = { aligned_alloc(8, gb_align8(g->len) + 8), g->len };

	memcpy(c.map, g->map, g->len);
	return c;
}

static inline struct ng_hdr *gb_hdr(struct gb_img *g)
{
	return g->map;
}

static inline struct ng_svc *gb_svc_at(struct gb_img *g, uint32_t i)
{
	return (struct ng_svc *)(void *)((char *)g->map + gb_hdr(g)->off_svc) + i;
}

static inline struct ng_pol *gb_pol_at(struct gb_img *g, uint32_t i)
{
	return (struct ng_pol *)(void *)((char *)g->map + gb_hdr(g)->off_pol) + i;
}

static inline uint32_t *gb_roff(struct gb_img *g)
{
	return (uint32_t *)(void *)((char *)g->map + gb_hdr(g)->off_rdep_off);
}

static inline uint32_t *gb_ridx(struct gb_img *g)
{
	return (uint32_t *)(void *)((char *)g->map + gb_hdr(g)->off_rdep_idx);
}

static inline char *gb_blob(struct gb_img *g)
{
	return (char *)g->map + gb_hdr(g)->off_blob;
}

#define GB_ONESHOT(n, sc)	{ .name = (n), .type = NG_TYPE_ONESHOT, .onfail = GB_DEFAULT, .script = (sc) }
#define GB_DAEMON(n, sc)	{ .name = (n), .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .script = (sc) }
#define GB_TARGET(n)		{ .name = (n), .type = NG_TYPE_TARGET, .onfail = GB_DEFAULT }
