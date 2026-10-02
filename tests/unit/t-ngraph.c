#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tap.h"

// ng_locale_env reads a fixed path, so it is pointed at a scratch file here
static const char *locale_path;

static int shim_open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/etc/locale.conf")) {
		if (!locale_path) {
			errno = ENOENT;
			return -1;
		}
		path = locale_path;
	}
	return open(path, flags);
}

#define open shim_open
#include "../../src/ngraph.c"
#undef open

#include "gbuild.h"

static void test_crc(void)
{
	static const struct {
		const char *in;
		size_t len;
		uint32_t crc;
	} v[] = {
		{ "", 0, 0x00000000u },
		{ "a", 1, 0xc1d04330u },
		{ "123456789", 9, 0xe3069283u },
		{ "The quick brown fox jumps over the lazy dog", 43, 0x22620404u },
	};
	unsigned char zeros[32] = { 0 }, ones[32], inc[32], dec[32];

	for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++)
		ok(ng_crc32c(v[k].in, v[k].len) == v[k].crc, "crc32c of \"%s\"", v[k].in);

	// rfc 3720 b.4
	memset(ones, 0xff, sizeof(ones));
	for (int k = 0; k < 32; k++) {
		inc[k] = (unsigned char)k;
		dec[k] = (unsigned char)(31 - k);
	}
	is_int(ng_crc32c(zeros, 32), 0x8a9136aau, "crc32c of 32 zero bytes");
	is_int(ng_crc32c(ones, 32), 0x62a8ab43u, "crc32c of 32 0xff bytes");
	is_int(ng_crc32c(inc, 32), 0x46dd794eu, "crc32c of 0..31");
	is_int(ng_crc32c(dec, 32), 0x113fdb5cu, "crc32c of 31..0");

	// the dispatcher may select sse4.2, the table result must match at every length and offset
	{
		unsigned char buf[600];
		int bad = 0;

		for (size_t k = 0; k < sizeof(buf); k++)
			buf[k] = (unsigned char)tap_rand();
		for (size_t at = 0; at < 9; at++)
			for (size_t len = 0; len + at <= sizeof(buf); len += 1 + len / 7) {
				uint32_t sw = ~(uint32_t)crc32c_feed_sw(~0u, buf + at, len);

				if (sw != ng_crc32c(buf + at, len) && bad++ < 5)
					tap_diag("mismatch at offset %zu length %zu", at, len);
			}
		ok(!bad, "crc32c dispatcher matches the table implementation");
	}

	// incremental feeding must equal a single pass
	{
		const char *s = "split anywhere and the running crc must not care";
		size_t n = strlen(s);
		int bad = 0;

		for (size_t cut = 0; cut <= n; cut++) {
			uint64_t c = crc32c_feed(~0u, s, cut);

			c = crc32c_feed(c, s + cut, n - cut);
			bad += ~(uint32_t)c != ng_crc32c(s, n);
		}
		ok(!bad, "crc32c is the same however the input is split");
	}
}

static struct gb_img sample(void)
{
	static const struct gb_svc sv[] = {
		GB_ONESHOT("fs", "fsck /"),
		GB_ONESHOT("udev", "exec udevd"),
		GB_ONESHOT("mounts", "mount -a"),
		GB_DAEMON("dbus", "exec dbus-daemon"),
		GB_TARGET("basic"),
		GB_DAEMON("getty", "exec agetty tty1"),
	};
	static const struct gb_edge e[] = {
		{ 0, 2 }, { 1, 3 }, { 2, 3 }, { 2, 4 }, { 3, 4 }, { 4, 5 },
	};

	return gb_build(sv, 6, e, 6);
}

static void test_image_crc(void)
{
	struct gb_img g = sample();
	struct ng_hdr *h = gb_hdr(&g);
	uint32_t c = h->crc32;

	is_int(ng_image_crc32c(g.map, g.len), c, "image crc skips its own field");
	h->crc32 = 0xdeadbeef;
	is_int(ng_image_crc32c(g.map, g.len), c, "image crc ignores what is stored in the field");
	h->crc32 = 0;
	is_int(ng_crc32c(g.map, g.len), c, "image crc is the crc of the image with the field zeroed");
	is_int(ng_image_crc32c(g.map, 63), 0, "a buffer shorter than the header has crc 0");
	free(g.map);
}

static void test_names(void)
{
	static const struct {
		const char *name, *why;
	} v[] = {
		{ "", "is empty" },
		{ ".hidden", "starts with a dot" },
		{ ".", "starts with a dot" },
		{ "foo~", "is an editor backup file" },
		{ "~", "is an editor backup file" },
		{ "#foo#", "is an editor autosave file" },
		{ "##", "is an editor autosave file" },
		{ "a/b", "contains a slash" },
		{ "a,b", "contains a comma" },
		{ "a b", "contains whitespace or a control character" },
		{ "a\tb", "contains whitespace or a control character" },
		{ "a\nb", "contains whitespace or a control character" },
		{ "a\x01" "b", "contains whitespace or a control character" },
		{ "a\x7f", "contains whitespace or a control character" },
		{ "#", NULL },
		{ "#foo", NULL },
		{ "foo#", NULL },
		{ "~foo", NULL },
		{ "a.b", NULL },
		{ "NetworkManager", NULL },
		{ "getty@tty1", NULL },
		{ "x:y=z", NULL },
		{ "caf\xc3\xa9", NULL },
	};
	char longname[300];

	for (size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		char what[96];

		snprintf(what, sizeof(what), "name problem for case %zu", k);
		is_str(ng_name_problem(v[k].name), v[k].why, what);
	}

	memset(longname, 'n', sizeof(longname));
	longname[255] = '\0';
	is_str(ng_name_problem(longname), NULL, "a 255 byte name is valid");
	longname[255] = 'n';
	longname[256] = '\0';
	is_str(ng_name_problem(longname), "is too long", "a 256 byte name is too long");

	ok(ng_reserved_name("unused"), "unused is reserved");
	ok(ng_reserved_name("depgraph"), "depgraph is reserved");
	ok(ng_reserved_name("depgraph.old"), "depgraph.old is reserved");
	ok(ng_reserved_name("depgraph.tmp"), "depgraph.tmp is reserved");
	ok(!ng_reserved_name("depgraph.new"), "depgraph.new is not reserved");
	ok(!ng_reserved_name("depgraphs"), "depgraphs is not reserved");
	ok(!ng_reserved_name("depgrap"), "depgrap is not reserved");
	ok(!ng_reserved_name("Unused"), "reserved names are case sensitive");
	ok(!ng_reserved_name("unused2"), "unused2 is not reserved");
	ok(!ng_reserved_name("old"), "old is not reserved");

	is_str(ng_typename(NG_TYPE_ONESHOT), "oneshot", "type 0 is oneshot");
	is_str(ng_typename(NG_TYPE_DAEMON), "daemon", "type 1 is daemon");
	is_str(ng_typename(NG_TYPE_TARGET), "target", "type 2 is target");
	is_str(ng_typename(3), "?", "type 3 is unknown");
	is_str(ng_onfailname(NG_ONFAIL_WARN), "warn", "onfail 0 is warn");
	is_str(ng_onfailname(NG_ONFAIL_STOP), "stop", "onfail 1 is stop");
	is_str(ng_onfailname(NG_ONFAIL_SHELL), "shell", "onfail 2 is shell");
	is_str(ng_onfailname(3), "?", "onfail 3 is unknown");
	is_str(ng_onfailname(NG_FLAG_RESTART | NG_ONFAIL_SHELL), "shell",
	       "onfail ignores the restart bit");
}

static void test_accessors(void)
{
	static const struct gb_svc sv[] = {
		GB_ONESHOT("a", "true"),
		GB_DAEMON("b", "exec sleep 1"),
		{ .name = "c", .type = NG_TYPE_DAEMON, .onfail = NG_ONFAIL_SHELL, .restart = 1,
		  .notify = 7, .script = "x",
		  .pol = { .start_ms = 1, .stop_ms = 2, .retry_ms = 3, .start_tries = 4,
			   .pflags = NG_PF_ORDER_ONLY } },
	};
	struct gb_img g = gb_build(sv, 3, NULL, 0);
	const void *m = g.map;

	is_str(ng_verify(m, g.len), NULL, "accessor sample verifies");
	is_int(ng_start_ms(m, 0), 90000, "start-timeout defaults to 90 s");
	is_int(ng_stop_ms(m, 0), 5000, "stop-timeout defaults to 5 s");
	is_int(ng_start_tries(m, 0), 1, "a oneshot defaults to 1 start try");
	is_int(ng_start_tries(m, 1), 2, "a daemon defaults to 2 start tries");
	is_int(ng_order_only(m, 0), 0, "deps defaults to uptime");
	is_int(ng_restart(m, 1), 0, "restart defaults to no");
	is_int(ng_notify(m, 1), 0, "notify defaults to none");
	is_int(ng_start_ms(m, 2), 1, "a set start-timeout is returned");
	is_int(ng_stop_ms(m, 2), 2, "a set stop-timeout is returned");
	is_int(ng_start_tries(m, 2), 4, "a set start-tries is returned");
	ok(ng_order_only(m, 2), "deps order is returned");
	ok(ng_restart(m, 2), "restart always is returned");
	is_int(ng_notify(m, 2), 7, "notify fd is returned");
	is_int(ng_onfail(m, 2), NG_ONFAIL_SHELL, "onfail is returned without the restart bit");
	is_str(ng_name(m, 2), "c", "name is returned");
	is_str(ng_script(m, 0), "true", "script is returned");
	free(g.map);
}

typedef void (*mutator)(struct gb_img *);

struct vcase {
	const char *what;
	const char *why;
	mutator fn;
	int recrc;
};

#define HDR(g)	gb_hdr(g)
#define SVC(g, i) gb_svc_at(g, i)
#define POL(g, i) gb_pol_at(g, i)

static void m_none(struct gb_img *g) { (void)g; }
static void m_magic(struct gb_img *g) { HDR(g)->magic ^= 1; }
static void m_version(struct gb_img *g) { HDR(g)->version = NG_VERSION - 1; }
static void m_version_new(struct gb_img *g) { HDR(g)->version = NG_VERSION + 1; }
static void m_total(struct gb_img *g) { HDR(g)->total_len++; }
static void m_nsvc_max(struct gb_img *g) { HDR(g)->n_svc = NG_MAX_SVC + 1; }
static void m_roots(struct gb_img *g) { HDR(g)->n_roots = HDR(g)->n_svc + 1; }
static void m_reserved(struct gb_img *g) { HDR(g)->reserved = 1; }
static void m_edges_nosvc(struct gb_img *g) { HDR(g)->n_svc = 0; HDR(g)->n_roots = 0; }
static void m_edges_max(struct gb_img *g) { HDR(g)->n_edges = 16; }
static void m_svc_oob(struct gb_img *g) { HDR(g)->off_svc = HDR(g)->total_len - 8; }
static void m_svc_align(struct gb_img *g) { HDR(g)->off_svc += 4; }
static void m_roff_oob(struct gb_img *g) { HDR(g)->off_rdep_off = HDR(g)->total_len; }
static void m_roff_align(struct gb_img *g) { HDR(g)->off_rdep_off += 2; }
static void m_ridx_oob(struct gb_img *g) { HDR(g)->off_rdep_idx = HDR(g)->total_len - 4; }
static void m_blob_oob(struct gb_img *g) { HDR(g)->blob_len += 1; }
static void m_pol_oob(struct gb_img *g) { HDR(g)->off_pol = HDR(g)->total_len - 4; }
static void m_overlap(struct gb_img *g) { HDR(g)->off_pol = HDR(g)->off_svc; }
static void m_overlap_hdr(struct gb_img *g) { HDR(g)->off_rdep_off = 56; }
static void m_crc(struct gb_img *g) { HDR(g)->crc32 ^= 0x10; }
static void m_payload(struct gb_img *g) { gb_blob(g)[0] ^= 0x20; }
static void m_blob_nul(struct gb_img *g) { gb_blob(g)[HDR(g)->blob_len - 1] = 'x'; }
static void m_roff0(struct gb_img *g) { gb_roff(g)[0] = 1; }
static void m_roffn(struct gb_img *g) { gb_roff(g)[HDR(g)->n_svc] = 5; }
static void m_roff_mono(struct gb_img *g) { gb_roff(g)[1] = 2; gb_roff(g)[2] = 1; }
static void m_ridx_range(struct gb_img *g) { gb_ridx(g)[5] = 99; }
static void m_ridx_back(struct gb_img *g) { gb_ridx(g)[0] = 0; }
static void m_ridx_sort(struct gb_img *g) { gb_ridx(g)[2] = 4; gb_ridx(g)[3] = 3; }
static void m_ndesc_dep(struct gb_img *g) { SVC(g, 2)->n_desc = 1; }
static void m_unmet_big(struct gb_img *g) { SVC(g, 5)->unmet = 6; }
static void m_ndesc_big(struct gb_img *g) { SVC(g, 0)->n_desc = 6; }
static void m_notify_range(struct gb_img *g) { SVC(g, 3)->notify_fd = 2; }
static void m_notify_big(struct gb_img *g) { SVC(g, 3)->notify_fd = 256; }
static void m_notify_type(struct gb_img *g) { SVC(g, 2)->notify_fd = 3; }
static void m_type(struct gb_img *g) { SVC(g, 5)->type = 3; }
static void m_onfail(struct gb_img *g) { SVC(g, 5)->flags = 3; }
static void m_flags(struct gb_img *g) { SVC(g, 5)->flags |= 0x08; }
static void m_warn_deps(struct gb_img *g) { SVC(g, 2)->flags = NG_ONFAIL_WARN; }
static void m_restart_type(struct gb_img *g) { SVC(g, 2)->flags |= NG_FLAG_RESTART; }
static void m_name_off(struct gb_img *g) { SVC(g, 1)->name_off = HDR(g)->blob_len; }
static void m_name_bad(struct gb_img *g) { gb_blob(g)[SVC(g, 1)->name_off] = '.'; }
static void m_root_unmet(struct gb_img *g) { SVC(g, 0)->unmet = 1; }
static void m_nonroot_unmet(struct gb_img *g) { HDR(g)->n_roots = 1; }
static void m_target_script(struct gb_img *g) { SVC(g, 4)->script_off = 0; }
static void m_script_off(struct gb_img *g) { SVC(g, 3)->script_off = HDR(g)->blob_len; }
static void m_tmo(struct gb_img *g) { POL(g, 1)->start_ms = NG_MAX_MS + 1; }
static void m_stop_tmo(struct gb_img *g) { POL(g, 1)->stop_ms = NG_MAX_MS + 1; }
static void m_tmo_max(struct gb_img *g) { POL(g, 1)->start_ms = NG_MAX_MS; }
static void m_tries(struct gb_img *g) { POL(g, 1)->start_tries = NG_MAX_TRIES + 1; }
static void m_tries_max(struct gb_img *g) { POL(g, 1)->start_tries = NG_MAX_TRIES; }
static void m_pflags(struct gb_img *g) { POL(g, 1)->pflags = 2; }
static void m_unmet_mismatch(struct gb_img *g) { SVC(g, 3)->unmet = 1; }
static void m_dup_name(struct gb_img *g) { SVC(g, 5)->name_off = SVC(g, 3)->name_off; }

// sample services by index, fs udev mounts dbus basic getty
static const struct vcase vcases[] = {
	{ "the sample verifies", NULL, m_none, 0 },
	{ "bad magic", "bad magic", m_magic, 0 },
	{ "older version", "version mismatch", m_version, 0 },
	{ "newer version", "version mismatch", m_version_new, 0 },
	{ "total_len differs from the file", "length mismatch", m_total, 0 },
	{ "too many services", "n_svc exceeds the supported maximum", m_nsvc_max, 0 },
	{ "more roots than services", "n_roots exceeds n_svc", m_roots, 0 },
	{ "reserved word set", "reserved header word is not zero", m_reserved, 0 },
	{ "edges with no services", "edges without services", m_edges_nosvc, 0 },
	{ "more edges than a dag holds", "n_edges exceeds what a forward-ordered graph can hold",
	  m_edges_max, 0 },
	{ "service table past the end", "service table out of bounds", m_svc_oob, 0 },
	{ "service table misaligned", "service table out of bounds", m_svc_align, 0 },
	{ "rdep offsets past the end", "rdep offsets out of bounds", m_roff_oob, 0 },
	{ "rdep offsets misaligned", "rdep offsets out of bounds", m_roff_align, 0 },
	{ "rdep indices past the end", "rdep indices out of bounds", m_ridx_oob, 0 },
	{ "blob past the end", "blob out of bounds", m_blob_oob, 0 },
	{ "policy table past the end", "policy table out of bounds", m_pol_oob, 0 },
	{ "policy table over the service table", "sections overlap", m_overlap, 0 },
	{ "rdep offsets over the header", "sections overlap", m_overlap_hdr, 0 },
	{ "crc field altered", "crc mismatch", m_crc, 0 },
	{ "payload altered", "crc mismatch", m_payload, 0 },
	{ "blob without a final nul", "blob not NUL-terminated", m_blob_nul, 1 },
	{ "rdep offsets start above 0", "rdep offsets do not start at 0", m_roff0, 1 },
	{ "rdep offsets end short", "rdep offsets do not end at n_edges", m_roffn, 1 },
	{ "rdep offsets go backwards", "rdep offsets not monotonic", m_roff_mono, 1 },
	{ "dependent index out of range", "rdep index out of range", m_ridx_range, 1 },
	{ "edge to itself", "edge runs backwards", m_ridx_back, 1 },
	{ "dependents unsorted", "rdep indices are not sorted and unique", m_ridx_sort, 1 },
	{ "n_desc below a dependent's", "n_desc is smaller than a dependent's descendant count",
	  m_ndesc_dep, 1 },
	{ "unmet of n_svc", "unmet exceeds the in-degree a service can have", m_unmet_big, 1 },
	{ "n_desc past the end", "n_desc exceeds the services that follow it", m_ndesc_big, 1 },
	{ "notify fd 2", "notify fd out of range", m_notify_range, 1 },
	{ "notify fd 256", "notify fd out of range", m_notify_big, 1 },
	{ "notify on a oneshot", "only a daemon can carry a notify fd", m_notify_type, 1 },
	{ "type 3", "unknown service type", m_type, 1 },
	{ "onfail 3", "unknown onfail policy", m_onfail, 1 },
	{ "flag bit 3", "unknown flag bits are set", m_flags, 1 },
	{ "onfail warn with dependents", "onfail warn on a service that has dependents", m_warn_deps, 1 },
	{ "restart on a oneshot", "only a daemon can be restarted", m_restart_type, 1 },
	{ "name offset past the blob", "name offset out of range", m_name_off, 1 },
	{ "name starting with a dot", "a service name is not a usable name", m_name_bad, 1 },
	{ "root with prerequisites", "root has nonzero unmet", m_root_unmet, 1 },
	{ "non-root without prerequisites", "non-root has zero unmet", m_nonroot_unmet, 1 },
	{ "target with a script", "target has a script", m_target_script, 1 },
	{ "script offset past the blob", "script offset out of range", m_script_off, 1 },
	{ "start-timeout above 24 h", "policy timeout out of range", m_tmo, 1 },
	{ "stop-timeout above 24 h", "policy timeout out of range", m_stop_tmo, 1 },
	{ "start-timeout of exactly 24 h", NULL, m_tmo_max, 1 },
	{ "start-tries 251", "policy start_tries out of range", m_tries, 1 },
	{ "start-tries 250", NULL, m_tries_max, 1 },
	{ "policy flag bit 1", "unknown policy flag bits are set", m_pflags, 1 },
	{ "unmet not the in-degree", "unmet does not match the in-degree of the edge list",
	  m_unmet_mismatch, 1 },
	{ "two services named alike", "two services share a name", m_dup_name, 1 },
};

static void test_verify_cases(void)
{
	struct gb_img base = sample();

	for (size_t k = 0; k < sizeof(vcases) / sizeof(*vcases); k++) {
		struct gb_img g = gb_copy(&base);

		vcases[k].fn(&g);
		if (vcases[k].recrc)
			gb_recrc(&g);
		is_str(ng_verify(g.map, g.len), vcases[k].why, vcases[k].what);
		free(g.map);
	}
	free(base.map);
}

static void test_verify_edges(void)
{
	struct gb_img g = sample(), c;
	char *odd;

	is_str(ng_verify(g.map, 63), "shorter than header", "63 bytes is shorter than the header");
	is_str(ng_verify(g.map, 0), "shorter than header", "0 bytes is shorter than the header");

	odd = aligned_alloc(8, gb_align8(g.len) + 16);
	memcpy(odd + 4, g.map, g.len);
	is_str(ng_verify(odd + 4, g.len), "image is not aligned for the header",
	       "a misaligned image is refused before it is read");
	free(odd);

	{
		struct gb_img e = gb_build(NULL, 0, NULL, 0);

		is_str(ng_verify(e.map, e.len), NULL, "an empty graph verifies");
		free(e.map);
	}

	// a name may be NG_MAX_NAME bytes, not one more
	{
		char nm[NG_MAX_NAME + 2];
		struct gb_svc sv[1] = { GB_TARGET(nm) };
		struct gb_img t;

		memset(nm, 'q', sizeof(nm));
		nm[NG_MAX_NAME] = '\0';
		t = gb_build(sv, 1, NULL, 0);
		is_str(ng_verify(t.map, t.len), NULL, "a 255 byte name verifies");
		free(t.map);
		nm[NG_MAX_NAME] = 'q';
		nm[NG_MAX_NAME + 1] = '\0';
		t = gb_build(sv, 1, NULL, 0);
		is_str(ng_verify(t.map, t.len), "name longer than the supported maximum",
		       "a 256 byte name is refused");
		free(t.map);
	}

	// two leaves under one service, each satisfied by n_desc 1 but not both
	{
		static const struct gb_svc sv[] = {
			GB_ONESHOT("a", ":"), GB_ONESHOT("b", ":"), GB_ONESHOT("c", ":"),
		};
		static const struct gb_edge e[] = { { 0, 1 }, { 0, 2 } };
		struct gb_img t = gb_build(sv, 3, e, 2);

		gb_svc_at(&t, 0)->n_desc = 1;
		gb_recrc(&t);
		is_str(ng_verify(t.map, t.len), "n_desc is smaller than the number of direct dependents",
		       "n_desc below the number of direct dependents");
		free(t.map);
	}

	// a runaway name offset into the last byte still finds the trailing nul
	c = gb_copy(&g);
	SVC(&c, 0)->name_off = HDR(&c)->blob_len - 1;
	gb_recrc(&c);
	is_str(ng_verify(c.map, c.len), "a service name is not a usable name",
	       "a name offset at the final nul is an empty name");
	free(c.map);

	// a script of exactly NG_MAX_SCRIPT bytes fits, one more does not
	{
		char *big = malloc(NG_MAX_SCRIPT + 2);
		struct gb_svc sv[1] = { GB_ONESHOT("big", big) };
		struct gb_img t;

		memset(big, ':', NG_MAX_SCRIPT + 1);
		big[NG_MAX_SCRIPT] = '\0';
		t = gb_build(sv, 1, NULL, 0);
		is_str(ng_verify(t.map, t.len), NULL, "a script of the maximum length verifies");
		free(t.map);
		big[NG_MAX_SCRIPT] = ':';
		big[NG_MAX_SCRIPT + 1] = '\0';
		t = gb_build(sv, 1, NULL, 0);
		is_str(ng_verify(t.map, t.len), "script longer than execve can carry",
		       "a script one byte over the maximum is refused");
		free(t.map);
		free(big);
	}

	// the edge bound n(n-1)/2 is exact for a complete dag
	{
		struct gb_svc sv[6];
		struct gb_edge e[15];
		uint32_t m = 0;
		struct gb_img t;

		for (uint32_t i = 0; i < 6; i++) {
			static const char *const nm[] = { "a", "b", "c", "d", "e", "f" };

			sv[i] = (struct gb_svc)GB_ONESHOT(nm[i], ":");
			for (uint32_t j = i + 1; j < 6; j++)
				e[m++] = (struct gb_edge){ i, j };
		}
		t = gb_build(sv, 6, e, m);
		is_str(ng_verify(t.map, t.len), NULL, "a complete dag of 6 services verifies");
		free(t.map);
	}
	free(g.map);
}

// whatever ng_verify accepts, ninit indexes without further checks
static const char *recheck(const void *map, size_t len)
{
	const struct ng_hdr *h = map;
	const struct ng_svc *sv = ng_svcs(map);
	const uint32_t *roff = ng_rdep_off(map), *ridx = ng_rdep_idx(map);
	uint32_t n = h->n_svc, i, j;

	if (h->total_len != len)
		return "length";
	for (i = 0; i < n; i++) {
		if (sv[i].name_off >= h->blob_len ||
		    !memchr(ng_blob(map) + sv[i].name_off, 0, h->blob_len - sv[i].name_off))
			return "name runs off the blob";
		if (sv[i].type != NG_TYPE_TARGET &&
		    (sv[i].script_off >= h->blob_len ||
		     !memchr(ng_blob(map) + sv[i].script_off, 0, h->blob_len - sv[i].script_off)))
			return "script runs off the blob";
		for (j = roff[i]; j < roff[i + 1]; j++)
			if (ridx[j] <= i || ridx[j] >= n)
				return "edge out of order";
		if ((i < h->n_roots) != (sv[i].unmet == 0))
			return "roots";
	}
	return NULL;
}

static void test_verify_fuzz(void)
{
	struct gb_img base = sample();
	unsigned accepted = 0, bad = 0, iters = 200000;

	for (unsigned it = 0; it < iters; it++) {
		struct gb_img g = gb_copy(&base);
		unsigned flips = 1 + tap_below(4);
		const char *why;

		for (unsigned f = 0; f < flips; f++) {
			size_t at = tap_below((uint32_t)g.len);

			if (tap_below(3))
				((unsigned char *)g.map)[at] ^= (unsigned char)(1u << tap_below(8));
			else
				((unsigned char *)g.map)[at] = (unsigned char)tap_rand();
		}
		// the crc is recomputed on 7 of 8 iterations so mutations reach the structural checks
		if (tap_below(8))
			gb_recrc(&g);
		why = ng_verify(g.map, g.len);
		if (!why) {
			const char *r = recheck(g.map, g.len);

			accepted++;
			if (r && bad++ < 5)
				tap_diag("iteration %u accepted an image that is unsafe: %s", it, r);
		}
		free(g.map);
	}
	ok(!bad, "%u mutated images, every accepted one (%u) is safe to index", iters, accepted);
	free(base.map);
}

static char locale_tmp[] = "/tmp/ninit-t-ngraph-locale.XXXXXX";

static void put_locale(const char *text, size_t len)
{
	int fd = open(locale_tmp, O_WRONLY | O_TRUNC | O_CLOEXEC);

	if (fd < 0 || write(fd, text, len) != (ssize_t)len)
		abort();
	close(fd);
}

static int run_locale(const char *text, char (*out)[NG_LOCALE_LEN], int max, const char **why)
{
	put_locale(text, strlen(text));
	locale_path = locale_tmp;
	memset(out, 0, (size_t)NG_LOCALE_MAX * NG_LOCALE_LEN);
	return ng_locale_env(out, max, why);
}

static void test_locale(void)
{
	char out[NG_LOCALE_MAX][NG_LOCALE_LEN];
	const char *why;
	int n, fd;

	fd = mkstemp(locale_tmp);
	if (fd < 0)
		abort();
	close(fd);

	locale_path = NULL;
	n = ng_locale_env(out, NG_LOCALE_MAX, &why);
	is_int(n, 0, "a missing locale.conf sets nothing");
	is_str(why, NULL, "a missing locale.conf is not a problem in itself");

	n = run_locale("LANG=en_US.UTF-8\n", out, NG_LOCALE_MAX, &why);
	is_int(n, 1, "one assignment");
	is_str(out[0], "LANG=en_US.UTF-8", "plain value");
	is_str(why, NULL, "no problem with a plain value");

	n = run_locale("LANG=\"de_DE.UTF-8\"\nLC_TIME='en_GB.UTF-8'  # time\n", out, NG_LOCALE_MAX, &why);
	is_int(n, 2, "quoted values");
	is_str(out[0], "LANG=de_DE.UTF-8", "double quotes are removed");
	is_str(out[1], "LC_TIME=en_GB.UTF-8", "single quotes and a trailing comment are removed");

	n = run_locale("# comment\n\n  \t\n  LANG = C.UTF-8  \r\nLC_ALL=de_DE@euro#x\n", out,
		       NG_LOCALE_MAX, &why);
	is_int(n, 2, "comments, blanks, spaces around = and a cr");
	is_str(out[0], "LANG=C.UTF-8", "spaces around = are tolerated");
	is_str(out[1], "LC_ALL=de_DE@euro", "@ is allowed and an unquoted # starts a comment");
	is_str(why, NULL, "nothing to report");

	n = run_locale("LANG=a\nLC_CTYPE=b\nLANG=c\n", out, NG_LOCALE_MAX, &why);
	is_int(n, 2, "a repeated variable is counted once");
	is_str(out[0], "LANG=c", "a later assignment wins, in place");
	is_str(out[1], "LC_CTYPE=b", "other assignments keep their order");

	n = run_locale("FOO=bar\nLANGUAGE=en\nLANG=C\n", out, NG_LOCALE_MAX, &why);
	is_int(n, 1, "only locale variables are taken");
	is_str(out[0], "LANG=C", "LANGUAGE and FOO are ignored");
	is_str(why, NULL, "unknown variables are not reported");

	n = run_locale("LANG\nLC_TIME=C\n", out, NG_LOCALE_MAX, &why);
	is_int(n, 1, "a line without = is skipped");
	is_str(why, "has a line that is not name=value", "and reported");

	n = run_locale("LANG=\"C\n", out, NG_LOCALE_MAX, &why);
	is_int(n, 0, "an unterminated quote is skipped");
	is_str(why, "has a value with no closing quote", "and reported");

	n = run_locale("LANG=\"C\" x\n", out, NG_LOCALE_MAX, &why);
	is_int(n, 0, "text after a quoted value is skipped");
	is_str(why, "has trailing text after a quoted value", "and reported");

	n = run_locale("LANG=$(reboot)\nLC_TIME=a/b\nLC_NAME=\n", out, NG_LOCALE_MAX, &why);
	is_int(n, 0, "values that are not locale names are skipped");
	is_str(why, "has a value that is not a locale name", "and reported");

	{
		char text[256];

		snprintf(text, sizeof(text), "LANG=%0122d\n", 0);
		n = run_locale(text, out, NG_LOCALE_MAX, &why);
		is_int(n, 1, "LANG= plus 122 bytes fits in 127");
		snprintf(text, sizeof(text), "LANG=%0123d\n", 0);
		n = run_locale(text, out, NG_LOCALE_MAX, &why);
		is_int(n, 0, "LANG= plus 123 bytes does not fit");
		is_str(why, "has a value that is too long", "and is reported");
	}

	n = run_locale("LANG=a\nLC_TIME=b\nLC_NAME=c\n", out, 2, &why);
	is_int(n, 2, "no more variables than the caller has room for");
	is_str(why, "sets more variables than ninit can carry", "the excess is reported");

	{
		static const char *const all[] = {
			"LANG", "LC_ALL", "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE",
			"LC_MONETARY", "LC_MESSAGES", "LC_PAPER", "LC_NAME", "LC_ADDRESS",
			"LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION",
		};
		char text[1024];
		size_t at = 0;

		for (size_t k = 0; k < 14; k++)
			at += (size_t)snprintf(text + at, sizeof(text) - at, "%s=C\n", all[k]);
		n = run_locale(text, out, NG_LOCALE_MAX, &why);
		is_int(n, 14, "all 14 locale variables are carried");
		is_str(out[13], "LC_IDENTIFICATION=C", "the last of them too");
	}

	n = run_locale("LANG=C", out, NG_LOCALE_MAX, &why);
	is_int(n, 1, "a last line without a newline counts");

	{
		char *big = malloc(9000);

		memset(big, '#', 9000);
		memcpy(big, "LANG=C\n", 7);
		big[8190] = '\n';
		big[8191] = '\0';
		n = run_locale(big, out, NG_LOCALE_MAX, &why);
		is_int(n, 1, "an 8191 byte file is read");
		is_str(why, NULL, "and is not too large");
		big[8191] = '\n';
		big[8192] = '\0';
		n = run_locale(big, out, NG_LOCALE_MAX, &why);
		is_int(n, 0, "an 8192 byte file is refused");
		is_str(why, "is larger than 8 KiB", "as too large");
		free(big);
	}

	n = run_locale("LANG=C\n", out, 0, &why);
	is_int(n, 0, "max 0 reads nothing");

	unlink(locale_tmp);
}

int main(void)
{
	test_crc();
	test_image_crc();
	test_names();
	test_accessors();
	test_verify_cases();
	test_verify_edges();
	test_verify_fuzz();
	test_locale();
	return tap_done();
}
