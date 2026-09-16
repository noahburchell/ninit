#include "ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#define NG_CRC_X86 1
#else
#define NG_CRC_X86 0
#endif

#if NG_CRC_X86
__attribute__((target("sse4.2")))
static uint64_t crc32c_feed_hw(uint64_t crc, const void *data, size_t len)
{
	const unsigned char *p = data;

#ifdef __x86_64__
	while (len >= 8) {
		uint64_t v;

		memcpy(&v, p, 8);
		crc = __builtin_ia32_crc32di(crc, v);
		p += 8;
		len -= 8;
	}
#else
	while (len >= 4) {
		uint32_t v;

		memcpy(&v, p, 4);
		crc = __builtin_ia32_crc32si((uint32_t)crc, v);
		p += 4;
		len -= 4;
	}
#endif
	while (len--)
		crc = __builtin_ia32_crc32qi((uint32_t)crc, *p++);

	return crc;
}
#endif

static uint32_t crc32c_table[256];

static void crc32c_table_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0x82f63b78u : c >> 1;
		crc32c_table[i] = c;
	}
}

static uint64_t crc32c_feed_sw(uint64_t crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	uint32_t c = (uint32_t)crc;

	if (!crc32c_table[1])
		crc32c_table_init();
	while (len--)
		c = crc32c_table[(c ^ *p++) & 0xff] ^ (c >> 8);

	return c;
}

static uint64_t crc32c_feed(uint64_t crc, const void *data, size_t len)
{
#if NG_CRC_X86
	static int have_hw = -1;

	if (have_hw < 0) {
		__builtin_cpu_init();
		have_hw = __builtin_cpu_supports("sse4.2");
	}
	if (have_hw)
		return crc32c_feed_hw(crc, data, len);
#endif
	return crc32c_feed_sw(crc, data, len);
}

uint32_t ng_crc32c(const void *data, size_t len)
{
	return ~(uint32_t)crc32c_feed(~0u, data, len);
}

// a buffer too short to hold the header has no image to checksum
uint32_t ng_image_crc32c(const void *map, size_t len)
{
	struct ng_hdr h;
	uint64_t crc;

	if (len < sizeof(h))
		return 0;
	memcpy(&h, map, sizeof(h));
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

const char *ng_name_problem(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;
	size_t n = strlen(name);

	if (!n)
		return "is empty";
	if (n > NG_MAX_NAME)
		return "is longer than a filename may be";
	if (name[0] == '.')
		return "starts with a dot; ninit never builds those";
	if (name[n - 1] == '~')
		return "looks like an editor backup; delete it or move it to unused/";
	if (n > 1 && name[0] == '#' && name[n - 1] == '#')
		return "looks like an editor autosave; delete it or move it to unused/";
	for (; *p; p++) {
		if (*p == '/')
			return "contains a slash; a service name is one filename";
		if (*p == ',')
			return "contains a comma; depon/depof could never name it";
		if (*p <= ' ' || *p == 0x7f)
			return "contains whitespace or a control character; depon/depof could never name it";
	}
	return NULL;
}

int ng_reserved_name(const char *name)
{
	if (!strcmp(name, "unused"))
		return 1;
	if (strncmp(name, "depgraph", 8))
		return 0;
	return !name[8] || !strcmp(name + 8, ".old") || !strcmp(name + 8, ".tmp");
}

static const char *const locale_vars[NG_LOCALE_MAX] = {
	"LANG", "LC_ALL", "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE",
	"LC_MONETARY", "LC_MESSAGES", "LC_PAPER", "LC_NAME", "LC_ADDRESS",
	"LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION",
};

static int locale_value_ok(const char *v)
{
	if (!*v)
		return 0;
	for (; *v; v++)
		if (!((*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z') ||
		      (*v >= '0' && *v <= '9') || *v == '_' || *v == '.' ||
		      *v == '-' || *v == '@'))
			return 0;
	return 1;
}

static void note(const char **why, const char *msg)
{
	if (!*why)
		*why = msg;
}

// parses /etc/locale.conf
int ng_locale_env(char (*out)[NG_LOCALE_LEN], int max, const char **why)
{
	char raw[NG_LOCALE_FILE];
	size_t len = 0;
	int seen[NG_LOCALE_MAX] = { 0 };
	int n = 0, fd;
	char *p;

	*why = NULL;
	if (max <= 0)
		return 0;
	fd = open(NG_LOCALE_CONF, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (fd < 0)
		return 0;
	for (;;) {
		ssize_t k = read(fd, raw + len, sizeof(raw) - 1 - len);

		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0) {
			close(fd);
			*why = "could not be read";
			return 0;
		}
		if (!k)
			break;
		len += (size_t)k;
		// the buffer is full
		if (len == sizeof(raw) - 1) {
			char extra;
			ssize_t e;

			do
				e = read(fd, &extra, 1);
			while (e < 0 && errno == EINTR);
			if (e > 0) {
				close(fd);
				*why = "is larger than 8 KiB";
				return 0;
			}
			break;
		}
	}
	close(fd);
	raw[len] = '\0';

	for (p = raw; *p; ) {
		char *nl = strchr(p, '\n'), *v, *name;
		size_t nlen;
		int k;

		if (nl)
			*nl = '\0';
		name = p;
		p = nl ? nl + 1 : p + strlen(p);

		while (*name == ' ' || *name == '\t')
			name++;
		if (!*name || *name == '#')
			continue;

		v = strchr(name, '=');
		if (!v) {
			note(why, "has a line that is not name=value");
			continue;
		}
		nlen = (size_t)(v - name);
		// tolerate spaces around the =
		while (nlen && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t'))
			nlen--;
		v++;
		while (*v == ' ' || *v == '\t')
			v++;

		for (k = 0; k < NG_LOCALE_MAX; k++)
			if (!strncmp(name, locale_vars[k], nlen) &&
			    locale_vars[k][nlen] == '\0')
				break;
		if (k == NG_LOCALE_MAX)
			continue;

		if (*v == '"' || *v == '\'') {
			char q = *v++;
			char *end = strchr(v, q), *rest;

			if (!end) {
				note(why, "has a value with no closing quote");
				continue;
			}
			*end = '\0';
			rest = end + 1;
			while (*rest == ' ' || *rest == '\t' || *rest == '\r')
				rest++;
			if (*rest && *rest != '#') {
				note(why, "has trailing text after a quoted value");
				continue;
			}
		} else {
			v[strcspn(v, " \t\r#")] = '\0';
		}

		if (!locale_value_ok(v)) {
			note(why, "has a value that is not a locale name");
			continue;
		}

		{
			size_t kn = strlen(locale_vars[k]), vn = strlen(v);
			char *dst;

			if (kn + 1 + vn >= NG_LOCALE_LEN) {
				note(why, "has a value that is too long");
				continue;
			}
			// a later line wins the way sourcing the file would
			if (seen[k]) {
				dst = out[seen[k] - 1];
			} else if (n == max) {
				note(why, "sets more variables than ninit can carry");
				continue;
			} else {
				dst = out[n];
				seen[k] = ++n;
			}
			memcpy(dst, locale_vars[k], kn);
			dst[kn] = '=';
			memcpy(dst + kn + 1, v, vn + 1);
		}
	}

	return n;
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

struct ng_span {
	uint64_t lo, hi;
};

// a checksum only proves the bytes are intact
static int spans_disjoint(const struct ng_span *v, unsigned n)
{
	unsigned i, j;

	for (i = 0; i < n; i++) {
		if (v[i].lo == v[i].hi)
			continue;
		for (j = i + 1; j < n; j++) {
			if (v[j].lo == v[j].hi)
				continue;
			if (v[i].lo < v[j].hi && v[j].lo < v[i].hi)
				return 0;
		}
	}
	return 1;
}

// the blobs trailing nul stops the search even for a runaway offset
static int str_fits(const char *blob, uint32_t blob_len, uint32_t off, uint32_t cap)
{
	uint32_t left;

	if (off >= blob_len)
		return 0;
	left = blob_len - off;
	if (left > cap + 1u)
		left = cap + 1u;
	return memchr(blob + off, '\0', left) != NULL;
}

struct name_ctx {
	const char *blob;
	const struct ng_svc *sv;
};

static int cmp_name_off(const void *a, const void *b, void *ctx)
{
	const struct name_ctx *c = ctx;
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return strcmp(c->blob + c->sv[x].name_off, c->blob + c->sv[y].name_off);
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
	// every accessor loads words straight out of the image
	if ((uintptr_t)map & (_Alignof(struct ng_hdr) - 1))
		return "image is not aligned for the header";
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
	if (h->reserved)
		return "reserved header word is not zero";
	if (!n && m)
		return "edges without services";
	if ((uint64_t)m > (uint64_t)n * (n - 1) / 2)
		return "n_edges exceeds what a forward-ordered graph can hold";

	if (!range_ok(h->off_svc, (uint64_t)n * sizeof(struct ng_svc), h->total_len, 8))
		return "service table out of bounds";
	if (!range_ok(h->off_rdep_off, ((uint64_t)n + 1) * 4, h->total_len, 4))
		return "rdep offsets out of bounds";
	if (!range_ok(h->off_rdep_idx, (uint64_t)m * 4, h->total_len, 4))
		return "rdep indices out of bounds";
	if (!range_ok(h->off_blob, h->blob_len, h->total_len, 1))
		return "blob out of bounds";
	if (!range_ok(h->off_pol, (uint64_t)n * sizeof(struct ng_pol), h->total_len, 4))
		return "policy table out of bounds";

	{
		const struct ng_span sp[] = {
			{ 0, sizeof(*h) },
			{ h->off_svc, (uint64_t)h->off_svc + (uint64_t)n * sizeof(struct ng_svc) },
			{ h->off_rdep_off, (uint64_t)h->off_rdep_off + ((uint64_t)n + 1) * 4 },
			{ h->off_rdep_idx, (uint64_t)h->off_rdep_idx + (uint64_t)m * 4 },
			{ h->off_pol, (uint64_t)h->off_pol + (uint64_t)n * sizeof(struct ng_pol) },
			{ h->off_blob, (uint64_t)h->off_blob + h->blob_len },
		};

		if (!spans_disjoint(sp, sizeof(sp) / sizeof(*sp)))
			return "sections overlap";
	}

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
			if (j > roff[i] && ridx[j] <= ridx[j - 1])
				return "rdep indices are not sorted and unique";
			if ((uint32_t)sv[ridx[j]].n_desc + 1u > s->n_desc)
				return "n_desc is smaller than a dependent's descendant count";
		}

		// an in-degree counts other services, so it cannot reach n_svc
		if (s->unmet >= n)
			return "unmet exceeds the in-degree a service can have";
		if (s->n_desc > n - 1 - i)
			return "n_desc exceeds the services that follow it";
		if (s->n_desc < roff[i + 1] - roff[i])
			return "n_desc is smaller than the number of direct dependents";
		if (s->notify_fd &&
		    (s->notify_fd < NG_NOTIFY_MIN || s->notify_fd > NG_NOTIFY_MAX))
			return "notify fd out of range";
		if (s->notify_fd && s->type != NG_TYPE_DAEMON)
			return "only a daemon can carry a notify fd";
		if (s->type > NG_TYPE_TARGET)
			return "unknown service type";
		if ((s->flags & NG_ONFAIL_MASK) > NG_ONFAIL_SHELL)
			return "unknown onfail policy";
		if (s->flags & ~NG_FLAG_MASK)
			return "unknown flag bits are set";
		if ((s->flags & NG_ONFAIL_MASK) == NG_ONFAIL_WARN && roff[i] != roff[i + 1])
			return "onfail warn on a service that has dependents";
		if ((s->flags & NG_FLAG_RESTART) && s->type != NG_TYPE_DAEMON)
			return "only a daemon can be restarted";
		if (s->name_off >= h->blob_len)
			return "name offset out of range";
		if (!str_fits(blob, h->blob_len, s->name_off, NG_MAX_NAME))
			return "name longer than the supported maximum";
		if (ng_name_problem(blob + s->name_off))
			return "a service name is not a usable name";
		if (i < h->n_roots && s->unmet != 0)
			return "root has nonzero unmet";
		if (i >= h->n_roots && s->unmet == 0)
			return "non-root has zero unmet";

		if (s->type == NG_TYPE_TARGET) {
			if (s->script_off != NG_NO_SCRIPT)
				return "target has a script";
		} else if (s->script_off >= h->blob_len) {
			return "script offset out of range";
		} else if (!str_fits(blob, h->blob_len, s->script_off, NG_MAX_SCRIPT)) {
			return "script longer than execve can carry";
		}

		{
			const struct ng_pol *p = ng_pol(map, i);

			if (p->start_ms > NG_MAX_MS || p->stop_ms > NG_MAX_MS)
				return "policy timeout out of range";
			if (p->start_tries > NG_MAX_TRIES)
				return "policy start_tries out of range";
			if (p->pflags & ~NG_PF_MASK)
				return "unknown policy flag bits are set";
		}
	}

	if (n) {
		struct name_ctx ctx = { blob, sv };
		uint32_t *scratch = calloc(n, sizeof(*scratch));
		const char *bad = NULL;

		if (!scratch)
			return "out of memory verifying the graph";

		for (i = 0; i < m; i++)
			scratch[ridx[i]]++;
		for (i = 0; i < n && !bad; i++)
			if (sv[i].unmet != scratch[i])
				bad = "unmet does not match the in-degree of the edge list";

		if (!bad) {
			for (i = 0; i < n; i++)
				scratch[i] = i;
			qsort_r(scratch, n, sizeof(*scratch), cmp_name_off, &ctx);
			for (i = 1; i < n; i++)
				if (!strcmp(blob + sv[scratch[i - 1]].name_off,
					    blob + sv[scratch[i]].name_off)) {
					bad = "two services share a name";
					break;
				}
		}
		free(scratch);
		if (bad)
			return bad;
	}

	return NULL;
}
