#include "init.h"
#include "parser/parser.h"
#include "util.h"
#include "../src/ngraph.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

void write_image(const struct build *b, const char *out, int dry, int srclock)
{
	struct src *srcs = b->srcs;
	struct edge *edges = b->edges;
	struct graph g = b->g;
	uint32_t *order = b->order;
	uint32_t n = b->n, m = b->m, nroots = b->nroots, i, j;
	uint64_t hash = b->hash;
	mode_t out_mode = b->out_mode;
	const char *why;
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
		pl[i].exec_off = NG_NO_EXEC;
		if (s->exec_pre.n) {
			sv[i].flags |= NG_FLAG_INTERP;
			pl[i].exec_off = blob_add(&blob, s->exec_pre.v[0]);
			for (j = 1; j < s->exec_pre.n; j++)
				blob_add(&blob, s->exec_pre.v[j]);
			blob_add(&blob, "");
			for (j = 0; j < s->exec_suf.n; j++)
				blob_add(&blob, s->exec_suf.v[j]);
			blob_add(&blob, "");
		}
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
		die("internal error: the built graph failed verification: %s", why);

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
			"ninitctl: warning: %u daemon%s with dependents %s no notify:, "
			"their dependents start on exec, not on readiness\n",
			vague, vague == 1 ? "" : "s", vague == 1 ? "has" : "have");
		for (i = 0; i < n; i++) {
			const struct src *s = &srcs[order[i]];

			if (s->type == NG_TYPE_DAEMON && !s->notify &&
			    roff[i] != roff[i + 1])
				fprintf(stderr, "ninitctl:   %s\n", s->name);
		}
	}
}
