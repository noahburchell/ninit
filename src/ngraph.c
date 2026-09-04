#include "ngraph.h"

#include <stdlib.h>
#include <string.h>

__attribute__((target("sse4.2")))
static uint64_t crc32c_feed(uint64_t crc, const void *data, size_t len)
{
	const unsigned char *p = data;

	while (len >= 8) {
		uint64_t v;

		memcpy(&v, p, 8);
		crc = __builtin_ia32_crc32di(crc, v);
		p += 8;
		len -= 8;
	}
	while (len--)
		crc = __builtin_ia32_crc32qi((uint32_t)crc, *p++);

	return crc;
}

uint32_t ng_crc32c(const void *data, size_t len)
{
	return ~(uint32_t)crc32c_feed(~0u, data, len);
}

uint32_t ng_image_crc32c(const void *map, size_t len)
{
	struct ng_hdr h = *(const struct ng_hdr *)map;
	uint64_t crc;

	h.crc32 = 0;
	crc = crc32c_feed(~0u, &h, sizeof(h));
	crc = crc32c_feed(crc, (const char *)map + sizeof(h), len - sizeof(h));

	return ~(uint32_t)crc;
}

const char *ng_typename(uint8_t type)
{
	switch (type) {
	case NG_TYPE_ONESHOT:	return "oneshot";
	case NG_TYPE_DAEMON:	return "daemon";
	case NG_TYPE_TARGET:	return "target";
	default:		return "?";
	}
}

const char *ng_onfailname(uint8_t policy)
{
	switch (policy & NG_ONFAIL_MASK) {
	case NG_ONFAIL_WARN:	return "warn";
	case NG_ONFAIL_STOP:	return "stop";
	case NG_ONFAIL_SHELL:	return "shell";
	default:		return "?";
	}
}

static int range_ok(uint32_t off, uint64_t bytes, uint32_t total, uint32_t align)
{
	if (off & (align - 1))
		return 0;
	return (uint64_t)off + bytes <= total;
}

const char *ng_verify(const void *map, size_t len)
{
	const struct ng_hdr *h = map;
	const struct ng_svc *sv;
	const uint32_t *roff, *ridx;
	const char *blob;
	uint32_t n, m, i;

	if (len < sizeof(*h))
		return "shorter than header";
	if (h->magic != NG_MAGIC)
		return "bad magic";
	if (h->version != NG_VERSION)
		return "version mismatch";
	if (h->total_len != len)
		return "length mismatch";

	n = h->n_svc;
	m = h->n_edges;
	if (n > NG_MAX_SVC)
		return "n_svc exceeds the supported maximum";
	if (h->n_roots > n)
		return "n_roots exceeds n_svc";
	if (h->reserved[0] || h->reserved[1])
		return "reserved header words are not zero";

	if (!range_ok(h->off_svc, (uint64_t)n * sizeof(struct ng_svc), h->total_len, 8))
		return "service table out of bounds";
	if (!range_ok(h->off_rdep_off, ((uint64_t)n + 1) * 4, h->total_len, 4))
		return "rdep offsets out of bounds";
	if (!range_ok(h->off_rdep_idx, (uint64_t)m * 4, h->total_len, 4))
		return "rdep indices out of bounds";
	if (!range_ok(h->off_blob, h->blob_len, h->total_len, 1))
		return "blob out of bounds";

	if (ng_image_crc32c(map, len) != h->crc32)
		return "crc mismatch (corrupt)";

	blob = ng_blob(map);
	if (h->blob_len && blob[h->blob_len - 1] != '\0')
		return "blob not NUL-terminated";

	roff = ng_rdep_off(map);
	ridx = ng_rdep_idx(map);
	sv = ng_svcs(map);

	if (n && roff[0] != 0)
		return "rdep offsets do not start at 0";
	if (roff[n] != m)
		return "rdep offsets do not end at n_edges";

	for (i = 0; i < n; i++) {
		const struct ng_svc *s = &sv[i];
		uint32_t j;

		if (roff[i] > roff[i + 1] || roff[i + 1] > m)
			return "rdep offsets not monotonic";
		for (j = roff[i]; j < roff[i + 1]; j++) {
			if (ridx[j] >= n)
				return "rdep index out of range";
			if (ridx[j] <= i)
				return "edge runs backwards (cycle, or not topologically ordered)";
		}

		if (s->unmet > n)
			return "unmet exceeds n_svc";
		if (s->n_desc >= n)
			return "n_desc exceeds n_svc";
		if (s->notify_fd &&
		    (s->notify_fd < NG_NOTIFY_MIN || s->notify_fd > NG_NOTIFY_MAX))
			return "notify fd out of range";
		if (s->notify_fd && s->type != NG_TYPE_DAEMON)
			return "only a daemon can carry a notify fd";
		if (s->type > NG_TYPE_TARGET)
			return "unknown service type";
		if ((s->flags & NG_ONFAIL_MASK) > NG_ONFAIL_SHELL)
			return "unknown onfail policy";
		if (s->name_off >= h->blob_len)
			return "name offset out of range";
		if (i < h->n_roots && s->unmet != 0)
			return "root has nonzero unmet";
		if (i >= h->n_roots && s->unmet == 0)
			return "non-root has zero unmet";

		if (s->type == NG_TYPE_TARGET) {
			if (s->script_off != NG_NO_SCRIPT)
				return "target has a script";
		} else if (s->script_off >= h->blob_len) {
			return "script offset out of range";
		}
	}

	if (n) {
		uint32_t *indeg = calloc(n, sizeof(*indeg));

		if (!indeg)
			return "out of memory verifying in-degrees";
		for (i = 0; i < m; i++)
			indeg[ridx[i]]++;
		for (i = 0; i < n; i++)
			if (sv[i].unmet != indeg[i]) {
				free(indeg);
				return "unmet does not match the in-degree of the edge list";
			}
		free(indeg);
	}

	return NULL;
}
