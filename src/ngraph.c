#include "ngraph.h"
#include "../help/io.h"
#include "../help/str.h"

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
__attribute__((target("sse4.2"))) static uint64_t crc32c_feed_hw(uint64_t crc, const void* data, size_t len) {
	const unsigned char* p = data;

#ifdef __x86_64__
	while(len >= 8) {
		uint64_t v;

		memcpy(&v, p, 8);
		crc = __builtin_ia32_crc32di(crc, v);
		p += 8;
		len -= 8;
	}
#else
	while(len >= 4) {
		uint32_t v;

		memcpy(&v, p, 4);
		crc = __builtin_ia32_crc32si((uint32_t)crc, v);
		p += 4;
		len -= 4;
	}
#endif
	while(len--) {
		crc = __builtin_ia32_crc32qi((uint32_t)crc, *p++);
	}

	return crc;
}
#endif

// the byte table of the reflected crc32c polynomial 0x82f63b78, t-ngraph checks it against
// the hardware instruction
static const uint32_t crc32c_table[256] = {
	// clang-format off
	0x00000000, 0xf26b8303, 0xe13b70f7, 0x1350f3f4, 0xc79a971f, 0x35f1141c,
	0x26a1e7e8, 0xd4ca64eb, 0x8ad958cf, 0x78b2dbcc, 0x6be22838, 0x9989ab3b,
	0x4d43cfd0, 0xbf284cd3, 0xac78bf27, 0x5e133c24, 0x105ec76f, 0xe235446c,
	0xf165b798, 0x030e349b, 0xd7c45070, 0x25afd373, 0x36ff2087, 0xc494a384,
	0x9a879fa0, 0x68ec1ca3, 0x7bbcef57, 0x89d76c54, 0x5d1d08bf, 0xaf768bbc,
	0xbc267848, 0x4e4dfb4b, 0x20bd8ede, 0xd2d60ddd, 0xc186fe29, 0x33ed7d2a,
	0xe72719c1, 0x154c9ac2, 0x061c6936, 0xf477ea35, 0xaa64d611, 0x580f5512,
	0x4b5fa6e6, 0xb93425e5, 0x6dfe410e, 0x9f95c20d, 0x8cc531f9, 0x7eaeb2fa,
	0x30e349b1, 0xc288cab2, 0xd1d83946, 0x23b3ba45, 0xf779deae, 0x05125dad,
	0x1642ae59, 0xe4292d5a, 0xba3a117e, 0x4851927d, 0x5b016189, 0xa96ae28a,
	0x7da08661, 0x8fcb0562, 0x9c9bf696, 0x6ef07595, 0x417b1dbc, 0xb3109ebf,
	0xa0406d4b, 0x522bee48, 0x86e18aa3, 0x748a09a0, 0x67dafa54, 0x95b17957,
	0xcba24573, 0x39c9c670, 0x2a993584, 0xd8f2b687, 0x0c38d26c, 0xfe53516f,
	0xed03a29b, 0x1f682198, 0x5125dad3, 0xa34e59d0, 0xb01eaa24, 0x42752927,
	0x96bf4dcc, 0x64d4cecf, 0x77843d3b, 0x85efbe38, 0xdbfc821c, 0x2997011f,
	0x3ac7f2eb, 0xc8ac71e8, 0x1c661503, 0xee0d9600, 0xfd5d65f4, 0x0f36e6f7,
	0x61c69362, 0x93ad1061, 0x80fde395, 0x72966096, 0xa65c047d, 0x5437877e,
	0x4767748a, 0xb50cf789, 0xeb1fcbad, 0x197448ae, 0x0a24bb5a, 0xf84f3859,
	0x2c855cb2, 0xdeeedfb1, 0xcdbe2c45, 0x3fd5af46, 0x7198540d, 0x83f3d70e,
	0x90a324fa, 0x62c8a7f9, 0xb602c312, 0x44694011, 0x5739b3e5, 0xa55230e6,
	0xfb410cc2, 0x092a8fc1, 0x1a7a7c35, 0xe811ff36, 0x3cdb9bdd, 0xceb018de,
	0xdde0eb2a, 0x2f8b6829, 0x82f63b78, 0x709db87b, 0x63cd4b8f, 0x91a6c88c,
	0x456cac67, 0xb7072f64, 0xa457dc90, 0x563c5f93, 0x082f63b7, 0xfa44e0b4,
	0xe9141340, 0x1b7f9043, 0xcfb5f4a8, 0x3dde77ab, 0x2e8e845f, 0xdce5075c,
	0x92a8fc17, 0x60c37f14, 0x73938ce0, 0x81f80fe3, 0x55326b08, 0xa759e80b,
	0xb4091bff, 0x466298fc, 0x1871a4d8, 0xea1a27db, 0xf94ad42f, 0x0b21572c,
	0xdfeb33c7, 0x2d80b0c4, 0x3ed04330, 0xccbbc033, 0xa24bb5a6, 0x502036a5,
	0x4370c551, 0xb11b4652, 0x65d122b9, 0x97baa1ba, 0x84ea524e, 0x7681d14d,
	0x2892ed69, 0xdaf96e6a, 0xc9a99d9e, 0x3bc21e9d, 0xef087a76, 0x1d63f975,
	0x0e330a81, 0xfc588982, 0xb21572c9, 0x407ef1ca, 0x532e023e, 0xa145813d,
	0x758fe5d6, 0x87e466d5, 0x94b49521, 0x66df1622, 0x38cc2a06, 0xcaa7a905,
	0xd9f75af1, 0x2b9cd9f2, 0xff56bd19, 0x0d3d3e1a, 0x1e6dcdee, 0xec064eed,
	0xc38d26c4, 0x31e6a5c7, 0x22b65633, 0xd0ddd530, 0x0417b1db, 0xf67c32d8,
	0xe52cc12c, 0x1747422f, 0x49547e0b, 0xbb3ffd08, 0xa86f0efc, 0x5a048dff,
	0x8ecee914, 0x7ca56a17, 0x6ff599e3, 0x9d9e1ae0, 0xd3d3e1ab, 0x21b862a8,
	0x32e8915c, 0xc083125f, 0x144976b4, 0xe622f5b7, 0xf5720643, 0x07198540,
	0x590ab964, 0xab613a67, 0xb831c993, 0x4a5a4a90, 0x9e902e7b, 0x6cfbad78,
	0x7fab5e8c, 0x8dc0dd8f, 0xe330a81a, 0x115b2b19, 0x020bd8ed, 0xf0605bee,
	0x24aa3f05, 0xd6c1bc06, 0xc5914ff2, 0x37faccf1, 0x69e9f0d5, 0x9b8273d6,
	0x88d28022, 0x7ab90321, 0xae7367ca, 0x5c18e4c9, 0x4f48173d, 0xbd23943e,
	0xf36e6f75, 0x0105ec76, 0x12551f82, 0xe03e9c81, 0x34f4f86a, 0xc69f7b69,
	0xd5cf889d, 0x27a40b9e, 0x79b737ba, 0x8bdcb4b9, 0x988c474d, 0x6ae7c44e,
	0xbe2da0a5, 0x4c4623a6, 0x5f16d052, 0xad7d5351,
	// clang-format on
};

static uint64_t crc32c_feed_sw(uint64_t crc, const void* data, size_t len) {
	const unsigned char* p = data;
	uint32_t c = (uint32_t)crc;

	while(len--) {
		c = crc32c_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	}

	return c;
}

static uint64_t crc32c_feed(uint64_t crc, const void* data, size_t len) {
#if NG_CRC_X86
#ifdef __SSE4_2__
	static const int have_hw = 1;
#else
	static int have_hw = -1;

	if(have_hw < 0) {
		__builtin_cpu_init();
		have_hw = __builtin_cpu_supports("sse4.2");
	}
#endif
	if(have_hw) {
		return crc32c_feed_hw(crc, data, len);
	}
#endif

	return crc32c_feed_sw(crc, data, len);
}

uint32_t ng_crc32c(const void* data, size_t len) {
	return ~(uint32_t)crc32c_feed(~0u, data, len);
}

// a buffer too short to hold the header has no image to checksum
uint32_t ng_image_crc32c(const void* map, size_t len) {
	struct ng_hdr h;
	uint64_t crc;

	if(len < sizeof(h)) {
		return 0;
	}

	memcpy(&h, map, sizeof(h));
	h.crc32 = 0;
	crc = crc32c_feed(~0u, &h, sizeof(h));
	crc = crc32c_feed(crc, (const char*)map + sizeof(h), len - sizeof(h));

	return ~(uint32_t)crc;
}

const char* ng_typename(uint8_t type) {
	switch(type) {
		case NG_TYPE_ONESHOT:
			return "oneshot";
		case NG_TYPE_DAEMON:
			return "daemon";
		case NG_TYPE_TARGET:
			return "target";
		default:
			return "?";
	}
}

const char* ng_name_problem(const char* name) {
	const unsigned char* p = (const unsigned char*)name;
	size_t n = strlen(name);

	if(!n) {
		return "is empty";
	}

	if(n > NG_MAX_NAME) {
		return "is too long";
	}

	if(name[0] == '.') {
		return "starts with a dot";
	}

	if(name[n - 1] == '~') {
		return "is an editor backup file";
	}

	if(n > 1 && name[0] == '#' && name[n - 1] == '#') {
		return "is an editor autosave file";
	}

	for(; *p; p++) {
		if(*p == '/') {
			return "contains a slash";
		}

		if(*p == ',') {
			return "contains a comma";
		}

		if(*p <= ' ' || *p == 0x7f) {
			return "contains whitespace or a control character";
		}
	}

	return NULL;
}

int ng_reserved_name(const char* name) {
	if(!strcmp(name, "unused")) {
		return 1;
	}

	if(strncmp(name, "depgraph", 8)) {
		return 0;
	}

	return !name[8] || !strcmp(name + 8, ".old") || !strcmp(name + 8, ".tmp");
}

static const str locale_vars[NG_LOCALE_MAX] = {
	STR_INIT("LANG"),
	STR_INIT("LC_ALL"),
	STR_INIT("LC_CTYPE"),
	STR_INIT("LC_NUMERIC"),
	STR_INIT("LC_TIME"),
	STR_INIT("LC_COLLATE"),
	STR_INIT("LC_MONETARY"),
	STR_INIT("LC_MESSAGES"),
	STR_INIT("LC_PAPER"),
	STR_INIT("LC_NAME"),
	STR_INIT("LC_ADDRESS"),
	STR_INIT("LC_TELEPHONE"),
	STR_INIT("LC_MEASUREMENT"),
	STR_INIT("LC_IDENTIFICATION"),
};

static int locale_value_ok(const char* v) {
	if(!*v) {
		return 0;
	}

	for(; *v; v++) {
		if(!((*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z')
		     || (*v >= '0' && *v <= '9') || *v == '_' || *v == '.'
		     || *v == '-' || *v == '@')) {
			return 0;
		}
	}

	return 1;
}

static void note(const char** why, const char* msg) {
	if(!*why) {
		*why = msg;
	}
}

// parses /etc/locale.conf
int ng_locale_env(char (*out)[NG_LOCALE_LEN], int max, const char** why) {
	*why = NULL;
	if(max <= 0) {
		return 0;
	}

	return ng_locale_fd(open(NG_LOCALE_CONF, O_RDONLY | O_CLOEXEC | O_NOCTTY), out, max, why);
}

int ng_locale_fd(int fd, char (*out)[NG_LOCALE_LEN], int max, const char** why) {
	char raw[NG_LOCALE_FILE], extra;
	int seen[NG_LOCALE_MAX] = { 0 };
	int n = 0;
	ssize_t len;
	char* p;

	*why = NULL;
	if(fd < 0) {
		return 0;
	}

	if(max <= 0) {
		close(fd);
		return 0;
	}

	len = read_full(fd, raw, sizeof(raw) - 1);
	if(len < 0) {
		close(fd);
		*why = "could not be read";
		return 0;
	}

	if((size_t)len == sizeof(raw) - 1 && read_full(fd, &extra, 1) > 0) {
		close(fd);
		*why = "is larger than 8 KiB";
		return 0;
	}

	close(fd);
	raw[len] = '\0';

	for(p = raw; *p;) {
		char* nl = strchr(p, '\n');
		char* v;
		char* name;
		size_t nlen;
		int k;

		if(nl) {
			*nl = '\0';
		}

		name = p;
		p = nl ? nl + 1 : p + strlen(p);

		while(*name == ' ' || *name == '\t') {
			name++;
		}

		if(!*name || *name == '#') {
			continue;
		}

		v = strchr(name, '=');
		if(!v) {
			note(why, "has a line that is not name=value");
			continue;
		}

		nlen = (size_t)(v - name);
		// tolerate spaces around the =
		while(nlen && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) {
			nlen--;
		}

		v++;
		while(*v == ' ' || *v == '\t') {
			v++;
		}

		for(k = 0; k < NG_LOCALE_MAX; k++) {
			if(str_eq((str){ name, nlen }, locale_vars[k])) {
				break;
			}
		}

		if(k == NG_LOCALE_MAX) {
			continue;
		}

		if(*v == '"' || *v == '\'') {
			char q = *v++;
			char* end = strchr(v, q);
			char* rest;

			if(!end) {
				note(why, "has a value with no closing quote");
				continue;
			}

			*end = '\0';
			rest = end + 1;
			while(*rest == ' ' || *rest == '\t' || *rest == '\r') {
				rest++;
			}

			if(*rest && *rest != '#') {
				note(why, "has trailing text after a quoted value");
				continue;
			}
		} else {
			v[strcspn(v, " \t\r#")] = '\0';
		}

		if(!locale_value_ok(v)) {
			note(why, "has a value that is not a locale name");
			continue;
		}

		{
			size_t kn = locale_vars[k].n, vn = strlen(v);
			char* dst;

			if(kn + 1 + vn >= NG_LOCALE_LEN) {
				note(why, "has a value that is too long");
				continue;
			}

			// a later line wins the way sourcing the file would
			if(seen[k]) {
				dst = out[seen[k] - 1];
			} else if(n == max) {
				note(why, "sets more variables than ninit can carry");
				continue;
			} else {
				dst = out[n];
				seen[k] = ++n;
			}

			memcpy(dst, locale_vars[k].p, kn);
			dst[kn] = '=';
			memcpy(dst + kn + 1, v, vn + 1);
		}
	}

	return n;
}

const char* ng_onfailname(uint8_t policy) {
	switch(policy & NG_ONFAIL_MASK) {
		case NG_ONFAIL_WARN:
			return "warn";
		case NG_ONFAIL_STOP:
			return "stop";
		case NG_ONFAIL_SHELL:
			return "shell";
		default:
			return "?";
	}
}

static int range_ok(uint32_t off, uint64_t bytes, uint32_t total, uint32_t align) {
	if(off & (align - 1)) {
		return 0;
	}

	return (uint64_t)off + bytes <= total;
}

struct ng_span {
	uint64_t lo, hi;
};

// a checksum only proves the bytes are intact
static int spans_disjoint(const struct ng_span* v, unsigned n) {
	unsigned i, j;

	for(i = 0; i < n; i++) {
		if(v[i].lo == v[i].hi) {
			continue;
		}

		for(j = i + 1; j < n; j++) {
			if(v[j].lo == v[j].hi) {
				continue;
			}

			if(v[i].lo < v[j].hi && v[j].lo < v[i].hi) {
				return 0;
			}
		}
	}

	return 1;
}

// the blobs trailing nul stops the search even for a runaway offset
static int str_fits(const char* blob, uint32_t blob_len, uint32_t off, uint32_t cap) {
	uint32_t left;

	if(off >= blob_len) {
		return 0;
	}

	left = blob_len - off;
	if(left > cap + 1u) {
		left = cap + 1u;
	}

	return memchr(blob + off, '\0', left) != NULL;
}

// the two string lists of an interpreter argv, each ended by an empty string
static const char* exec_problem(const char* blob, uint32_t blob_len, uint32_t off) {
	uint32_t args = 0, list;

	if(off >= blob_len) {
		return "exec offset out of range";
	}

	if(blob[off] != '/') {
		return "interpreter is not an absolute path";
	}

	if(!str_fits(blob, blob_len, off, NG_MAX_INTERP)) {
		return "interpreter path longer than the supported maximum";
	}

	for(list = 0; list < 2; list++) {
		while(blob[off]) {
			if(++args > NG_MAX_EXEC_ARGS) {
				return "too many interpreter arguments";
			}

			if(!str_fits(blob, blob_len, off, NG_MAX_EXEC_ARG)) {
				return "interpreter argument longer than the supported maximum";
			}

			off += (uint32_t)strlen(blob + off) + 1;
			if(off >= blob_len) {
				return "interpreter arguments run past the string table";
			}
		}

		off++;
		if(list == 0 && off >= blob_len) {
			return "interpreter arguments run past the string table";
		}
	}

	return NULL;
}

// the argv of a direct exec, the command and its arguments ended by an empty string
static const char* direct_problem(const char* blob, uint32_t blob_len, uint32_t off) {
	uint32_t args = 1;

	if(off >= blob_len) {
		return "exec offset out of range";
	}

	if(!blob[off]) {
		return "direct argv is empty";
	}

	if(!str_fits(blob, blob_len, off, NG_MAX_INTERP)) {
		return "command longer than the supported maximum";
	}

	if(blob[off] != '/' && strchr(blob + off, '/')) {
		return "command with a slash is not an absolute path";
	}

	for(;;) {
		off += (uint32_t)strlen(blob + off) + 1;
		if(off >= blob_len) {
			return "direct arguments run past the string table";
		}

		if(!blob[off]) {
			return NULL;
		}

		if(++args > NG_MAX_EXEC_ARGS) {
			return "too many direct arguments";
		}

		if(!str_fits(blob, blob_len, off, NG_MAX_EXEC_ARG)) {
			return "direct argument longer than the supported maximum";
		}
	}
}

static uint32_t name_hash(const char* s) {
	uint32_t h = 2166136261u;

	while(*s) {
		h = (h ^ (unsigned char)*s++) * 16777619u;
	}

	return h;
}

const char* ng_verify(const void* map, size_t len) {
	const struct ng_hdr* h = map;
	const struct ng_svc* sv;
	const uint32_t* roff;
	const uint32_t* ridx;
	const char* blob;
	uint32_t n, m, i;

	if(len < sizeof(*h)) {
		return "shorter than header";
	}

	// every accessor loads words straight out of the image
	if((uintptr_t)map & (_Alignof(struct ng_hdr) - 1)) {
		return "image is not aligned for the header";
	}

	if(h->magic != NG_MAGIC) {
		return "bad magic";
	}

	if(h->version != NG_VERSION) {
		return "version mismatch";
	}

	if(h->total_len != len) {
		return "length mismatch";
	}

	n = h->n_svc;
	m = h->n_edges;
	if(n > NG_MAX_SVC) {
		return "n_svc exceeds the supported maximum";
	}

	if(h->n_roots > n) {
		return "n_roots exceeds n_svc";
	}

	if(h->reserved) {
		return "reserved header word is not zero";
	}

	if(!n && m) {
		return "edges without services";
	}

	if((uint64_t)m > (uint64_t)n * (n - 1) / 2) {
		return "n_edges exceeds what a forward-ordered graph can hold";
	}

	if(!range_ok(h->off_svc, (uint64_t)n * sizeof(struct ng_svc), h->total_len, 8)) {
		return "service table out of bounds";
	}

	if(!range_ok(h->off_rdep_off, ((uint64_t)n + 1) * 4, h->total_len, 4)) {
		return "rdep offsets out of bounds";
	}

	if(!range_ok(h->off_rdep_idx, (uint64_t)m * 4, h->total_len, 4)) {
		return "rdep indices out of bounds";
	}

	if(!range_ok(h->off_blob, h->blob_len, h->total_len, 1)) {
		return "blob out of bounds";
	}

	if(!range_ok(h->off_pol, (uint64_t)n * sizeof(struct ng_pol), h->total_len, 4)) {
		return "policy table out of bounds";
	}

	{
		const struct ng_span sp[] = {
			{ 0, sizeof(*h) },
			{ h->off_svc, (uint64_t)h->off_svc + (uint64_t)n * sizeof(struct ng_svc) },
			{ h->off_rdep_off, (uint64_t)h->off_rdep_off + ((uint64_t)n + 1) * 4 },
			{ h->off_rdep_idx, (uint64_t)h->off_rdep_idx + (uint64_t)m * 4 },
			{ h->off_pol, (uint64_t)h->off_pol + (uint64_t)n * sizeof(struct ng_pol) },
			{ h->off_blob, (uint64_t)h->off_blob + h->blob_len },
		};

		if(!spans_disjoint(sp, sizeof(sp) / sizeof(*sp))) {
			return "sections overlap";
		}
	}

	if(ng_image_crc32c(map, len) != h->crc32) {
		return "crc mismatch";
	}

	blob = ng_blob(map);
	if(h->blob_len && blob[h->blob_len - 1] != '\0') {
		return "blob not NUL-terminated";
	}

	roff = ng_rdep_off(map);
	ridx = ng_rdep_idx(map);
	sv = ng_svcs(map);

	if(n && roff[0] != 0) {
		return "rdep offsets do not start at 0";
	}

	if(roff[n] != m) {
		return "rdep offsets do not end at n_edges";
	}

	for(i = 0; i < n; i++) {
		const struct ng_svc* s = &sv[i];
		uint32_t j;

		if(roff[i] > roff[i + 1] || roff[i + 1] > m) {
			return "rdep offsets not monotonic";
		}

		for(j = roff[i]; j < roff[i + 1]; j++) {
			if(ridx[j] >= n) {
				return "rdep index out of range";
			}

			if(ridx[j] <= i) {
				return "edge runs backwards";
			}

			if(j > roff[i] && ridx[j] <= ridx[j - 1]) {
				return "rdep indices are not sorted and unique";
			}

			if((uint32_t)sv[ridx[j]].n_desc + 1u > s->n_desc) {
				return "n_desc is smaller than a dependent's descendant count";
			}
		}

		// an in-degree counts other services, so it cannot reach n_svc
		if(s->unmet >= n) {
			return "unmet exceeds the in-degree a service can have";
		}

		if(s->n_desc > n - 1 - i) {
			return "n_desc exceeds the services that follow it";
		}

		if(s->n_desc < roff[i + 1] - roff[i]) {
			return "n_desc is smaller than the number of direct dependents";
		}

		if(s->notify_fd
		   && (s->notify_fd < NG_NOTIFY_MIN || s->notify_fd > NG_NOTIFY_MAX)) {
			return "notify fd out of range";
		}

		if(s->notify_fd && s->type != NG_TYPE_DAEMON) {
			return "only a daemon can carry a notify fd";
		}

		if(s->type > NG_TYPE_TARGET) {
			return "unknown service type";
		}

		if((s->flags & NG_ONFAIL_MASK) > NG_ONFAIL_SHELL) {
			return "unknown onfail policy";
		}

		if(s->flags & ~NG_FLAG_MASK) {
			return "unknown flag bits are set";
		}

		if((s->flags & NG_ONFAIL_MASK) == NG_ONFAIL_WARN && roff[i] != roff[i + 1]) {
			return "onfail warn on a service that has dependents";
		}

		if((s->flags & NG_FLAG_RESTART) && s->type != NG_TYPE_DAEMON) {
			return "only a daemon can be restarted";
		}

		if(s->name_off >= h->blob_len) {
			return "name offset out of range";
		}

		if(!str_fits(blob, h->blob_len, s->name_off, NG_MAX_NAME)) {
			return "name longer than the supported maximum";
		}

		if(ng_name_problem(blob + s->name_off)) {
			return "a service name is not a usable name";
		}

		if(i < h->n_roots && s->unmet != 0) {
			return "root has nonzero unmet";
		}

		if(i >= h->n_roots && s->unmet == 0) {
			return "non-root has zero unmet";
		}

		if(s->type == NG_TYPE_TARGET) {
			if(s->script_off != NG_NO_SCRIPT) {
				return "target has a script";
			}
		} else if(s->script_off >= h->blob_len) {
			return "script offset out of range";
		} else if(!str_fits(blob, h->blob_len, s->script_off, NG_MAX_SCRIPT)) {
			return "script longer than execve can carry";
		}

		{
			const struct ng_pol* p = ng_pol(map, i);

			if(p->start_ms > NG_MAX_MS || p->stop_ms > NG_MAX_MS) {
				return "policy timeout out of range";
			}

			if(p->start_tries > NG_MAX_TRIES) {
				return "policy start_tries out of range";
			}

			if(p->pflags & ~NG_PF_MASK) {
				return "unknown policy flag bits are set";
			}

			if((s->flags & (NG_FLAG_INTERP | NG_FLAG_DIRECT)) == (NG_FLAG_INTERP | NG_FLAG_DIRECT)) {
				return "a service is both interpreted and direct";
			}

			if(s->flags & NG_FLAG_INTERP) {
				const char* why;

				if(s->type == NG_TYPE_TARGET) {
					return "a target has an interpreter";
				}

				why = exec_problem(blob, h->blob_len, p->exec_off);
				if(why) {
					return why;
				}
			} else if(s->flags & NG_FLAG_DIRECT) {
				const char* why;

				if(s->type == NG_TYPE_TARGET) {
					return "a target has a direct argv";
				}

				why = direct_problem(blob, h->blob_len, p->exec_off);
				if(why) {
					return why;
				}
			} else if(p->exec_off != NG_NO_EXEC) {
				return "exec offset set without the interpreter or direct flag";
			}
		}
	}

	if(n) {
		uint32_t hsize = 64;
		uint32_t* scratch;
		uint32_t* tab;
		const char* bad = NULL;

		while(hsize < 2 * n) {
			hsize *= 2;
		}

		scratch = calloc((size_t)n + hsize, sizeof(*scratch));
		if(!scratch) {
			return "out of memory verifying the graph";
		}

		tab = scratch + n;

		for(i = 0; i < m; i++) {
			scratch[ridx[i]]++;
		}

		for(i = 0; i < n && !bad; i++) {
			if(sv[i].unmet != scratch[i]) {
				bad = "unmet does not match the in-degree of the edge list";
			}
		}

		for(i = 0; i < n && !bad; i++) {
			const char* nm = blob + sv[i].name_off;
			uint32_t at = name_hash(nm) & (hsize - 1);

			for(;;) {
				uint32_t slot = tab[at];

				if(!slot) {
					tab[at] = i + 1;
					break;
				}

				if(!strcmp(blob + sv[slot - 1].name_off, nm)) {
					bad = "two services share a name";
					break;
				}

				at = (at + 1) & (hsize - 1);
			}
		}

		free(scratch);
		if(bad) {
			return bad;
		}
	}

	return NULL;
}
