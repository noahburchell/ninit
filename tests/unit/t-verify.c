#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tap.h"

#include "../../src/ngraph.c"

#include "gbuild.h"

typedef void (*mutator)(struct gb_img*);

struct vcase {
	const char* what;
	const char* why;
	mutator     fn;
	int         recrc;
};

#define HDR(g)	  gb_hdr(g)
#define SVC(g, i) gb_svc_at(g, i)
#define POL(g, i) gb_pol_at(g, i)

static void m_none(struct gb_img* g) {
	(void)g;
}
static void m_magic(struct gb_img* g) {
	HDR(g)->magic ^= 1;
}
static void m_version(struct gb_img* g) {
	HDR(g)->version = NG_VERSION - 1;
}
static void m_version_new(struct gb_img* g) {
	HDR(g)->version = NG_VERSION + 1;
}
static void m_total(struct gb_img* g) {
	HDR(g)->total_len++;
}
static void m_nsvc_max(struct gb_img* g) {
	HDR(g)->n_svc = NG_MAX_SVC + 1;
}
static void m_roots(struct gb_img* g) {
	HDR(g)->n_roots = HDR(g)->n_svc + 1;
}
static void m_reserved(struct gb_img* g) {
	HDR(g)->reserved = 1;
}
static void m_edges_nosvc(struct gb_img* g) {
	HDR(g)->n_svc = 0;
	HDR(g)->n_roots = 0;
}
static void m_edges_max(struct gb_img* g) {
	HDR(g)->n_edges = 16;
}
static void m_svc_oob(struct gb_img* g) {
	HDR(g)->off_svc = HDR(g)->total_len - 8;
}
static void m_svc_align(struct gb_img* g) {
	HDR(g)->off_svc += 4;
}
static void m_roff_oob(struct gb_img* g) {
	HDR(g)->off_rdep_off = HDR(g)->total_len;
}
static void m_roff_align(struct gb_img* g) {
	HDR(g)->off_rdep_off += 2;
}
static void m_ridx_oob(struct gb_img* g) {
	HDR(g)->off_rdep_idx = HDR(g)->total_len - 4;
}
static void m_blob_oob(struct gb_img* g) {
	HDR(g)->blob_len += 1;
}
static void m_pol_oob(struct gb_img* g) {
	HDR(g)->off_pol = HDR(g)->total_len - 4;
}
static void m_overlap(struct gb_img* g) {
	HDR(g)->off_pol = HDR(g)->off_svc;
}
static void m_overlap_hdr(struct gb_img* g) {
	HDR(g)->off_rdep_off = 56;
}
static void m_crc(struct gb_img* g) {
	HDR(g)->crc32 ^= 0x10;
}
static void m_payload(struct gb_img* g) {
	gb_blob(g)[0] ^= 0x20;
}
static void m_blob_nul(struct gb_img* g) {
	gb_blob(g)[HDR(g)->blob_len - 1] = 'x';
}
static void m_roff0(struct gb_img* g) {
	gb_roff(g)[0] = 1;
}
static void m_roffn(struct gb_img* g) {
	gb_roff(g)[HDR(g)->n_svc] = 5;
}
static void m_roff_mono(struct gb_img* g) {
	gb_roff(g)[1] = 2;
	gb_roff(g)[2] = 1;
}
static void m_ridx_range(struct gb_img* g) {
	gb_ridx(g)[5] = 99;
}
static void m_ridx_back(struct gb_img* g) {
	gb_ridx(g)[0] = 0;
}
static void m_ridx_sort(struct gb_img* g) {
	gb_ridx(g)[2] = 4;
	gb_ridx(g)[3] = 3;
}
static void m_ndesc_dep(struct gb_img* g) {
	SVC(g, 2)->n_desc = 1;
}
static void m_unmet_big(struct gb_img* g) {
	SVC(g, 5)->unmet = 6;
}
static void m_ndesc_big(struct gb_img* g) {
	SVC(g, 0)->n_desc = 6;
}
static void m_notify_range(struct gb_img* g) {
	SVC(g, 3)->notify_fd = 2;
}
static void m_notify_big(struct gb_img* g) {
	SVC(g, 3)->notify_fd = 256;
}
static void m_notify_type(struct gb_img* g) {
	SVC(g, 2)->notify_fd = 3;
}
static void m_type(struct gb_img* g) {
	SVC(g, 5)->type = 3;
}
static void m_onfail(struct gb_img* g) {
	SVC(g, 5)->flags = 3;
}
static void m_flags(struct gb_img* g) {
	SVC(g, 5)->flags |= 0x20;
}
static void m_interp_noexec(struct gb_img* g) {
	SVC(g, 1)->flags |= NG_FLAG_INTERP;
}
static void m_exec_noflag(struct gb_img* g) {
	POL(g, 1)->exec_off = 0;
}
static void m_interp_target(struct gb_img* g) {
	SVC(g, 4)->flags |= NG_FLAG_INTERP;
}
static void m_direct_noexec(struct gb_img* g) {
	SVC(g, 1)->flags |= NG_FLAG_DIRECT;
}
static void m_direct_target(struct gb_img* g) {
	SVC(g, 4)->flags |= NG_FLAG_DIRECT;
}
static void m_direct_interp(struct gb_img* g) {
	SVC(g, 3)->flags |= NG_FLAG_DIRECT;
}
static void m_direct_empty(struct gb_img* g) {
	gb_blob(g)[POL(g, 5)->exec_off] = '\0';
}
static void m_warn_deps(struct gb_img* g) {
	SVC(g, 2)->flags = NG_ONFAIL_WARN;
}
static void m_restart_type(struct gb_img* g) {
	SVC(g, 2)->flags |= NG_FLAG_RESTART;
}
static void m_name_off(struct gb_img* g) {
	SVC(g, 1)->name_off = HDR(g)->blob_len;
}
static void m_name_bad(struct gb_img* g) {
	gb_blob(g)[SVC(g, 1)->name_off] = '.';
}
static void m_root_unmet(struct gb_img* g) {
	SVC(g, 0)->unmet = 1;
}
static void m_nonroot_unmet(struct gb_img* g) {
	HDR(g)->n_roots = 1;
}
static void m_target_script(struct gb_img* g) {
	SVC(g, 4)->script_off = 0;
}
static void m_script_off(struct gb_img* g) {
	SVC(g, 3)->script_off = HDR(g)->blob_len;
}
static void m_tmo(struct gb_img* g) {
	POL(g, 1)->start_ms = NG_MAX_MS + 1;
}
static void m_stop_tmo(struct gb_img* g) {
	POL(g, 1)->stop_ms = NG_MAX_MS + 1;
}
static void m_tmo_max(struct gb_img* g) {
	POL(g, 1)->start_ms = NG_MAX_MS;
}
static void m_tries(struct gb_img* g) {
	POL(g, 1)->start_tries = NG_MAX_TRIES + 1;
}
static void m_tries_max(struct gb_img* g) {
	POL(g, 1)->start_tries = NG_MAX_TRIES;
}
static void m_pflags(struct gb_img* g) {
	POL(g, 1)->pflags = 2;
}
static void m_unmet_mismatch(struct gb_img* g) {
	SVC(g, 3)->unmet = 1;
}
static void m_dup_name(struct gb_img* g) {
	SVC(g, 5)->name_off = SVC(g, 3)->name_off;
}

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
	{ "flag bit 5", "unknown flag bits are set", m_flags, 1 },
	{ "the interpreter flag without an argv", "exec offset out of range", m_interp_noexec, 1 },
	{ "an argv without the interpreter flag", "exec offset set without the interpreter or direct flag",
	  m_exec_noflag, 1 },
	{ "a target with an interpreter", "a target has an interpreter", m_interp_target, 1 },
	{ "the direct flag without an argv", "exec offset out of range", m_direct_noexec, 1 },
	{ "a target with a direct argv", "a target has a direct argv", m_direct_target, 1 },
	{ "the direct and interpreter flags together", "a service is both interpreted and direct",
	  m_direct_interp, 1 },
	{ "a direct argv without a command", "direct argv is empty", m_direct_empty, 1 },
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

static void test_verify_cases(void) {
	struct gb_img base = gb_sample();

	for(size_t k = 0; k < sizeof(vcases) / sizeof(*vcases); k++) {
		struct gb_img g = gb_copy(&base);

		vcases[k].fn(&g);
		if(vcases[k].recrc) {
			gb_recrc(&g);
		}

		is_str(ng_verify(g.map, g.len), vcases[k].why, vcases[k].what);
		free(g.map);
	}

	free(base.map);
}

static void test_verify_edges(void) {
	struct gb_img g = gb_sample(), c;
	char* odd;

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
			GB_ONESHOT("a", ":"),
			GB_ONESHOT("b", ":"),
			GB_ONESHOT("c", ":"),
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
		char* big = malloc(NG_MAX_SCRIPT + 2);
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

		for(uint32_t i = 0; i < 6; i++) {
			static const char* const nm[] = { "a", "b", "c", "d", "e", "f" };

			sv[i] = (struct gb_svc)GB_ONESHOT(nm[i], ":");
			for(uint32_t j = i + 1; j < 6; j++) {
				e[m++] = (struct gb_edge){ i, j };
			}
		}

		t = gb_build(sv, 6, e, m);
		is_str(ng_verify(t.map, t.len), NULL, "a complete dag of 6 services verifies");
		free(t.map);
	}

	free(g.map);
}

// whatever ng_verify accepts, ninit indexes without further checks
static const char* recheck(const void* map, size_t len) {
	const struct ng_hdr* h = map;
	const struct ng_svc* sv = ng_svcs(map);
	const uint32_t* roff = ng_rdep_off(map);
	const uint32_t* ridx = ng_rdep_idx(map);
	uint32_t n = h->n_svc, i, j;

	if(h->total_len != len) {
		return "length";
	}

	for(i = 0; i < n; i++) {
		if(sv[i].name_off >= h->blob_len
		   || !memchr(ng_blob(map) + sv[i].name_off, 0, h->blob_len - sv[i].name_off)) {
			return "name runs off the blob";
		}

		if(sv[i].type != NG_TYPE_TARGET
		   && (sv[i].script_off >= h->blob_len
		       || !memchr(ng_blob(map) + sv[i].script_off, 0, h->blob_len - sv[i].script_off))) {
			return "script runs off the blob";
		}

		for(j = roff[i]; j < roff[i + 1]; j++) {
			if(ridx[j] <= i || ridx[j] >= n) {
				return "edge out of order";
			}
		}

		if((i < h->n_roots) != (sv[i].unmet == 0)) {
			return "roots";
		}

		if(sv[i].flags & (NG_FLAG_INTERP | NG_FLAG_DIRECT)) {
			uint32_t off = ng_pol(map, i)->exec_off, k = 0;
			int lists = sv[i].flags & NG_FLAG_INTERP ? 2 : 1;

			// the walk child_exec does
			for(int list = 0; list < lists; list++) {
				while(off < h->blob_len && ng_blob(map)[off]) {
					const char* z = memchr(ng_blob(map) + off, 0, h->blob_len - off);

					if(!z) {
						return "an argument runs off the blob";
					}

					off = (uint32_t)(z - ng_blob(map)) + 1;
					k++;
				}

				if(off >= h->blob_len) {
					return "an argv runs off the blob";
				}

				off++;
			}

			if(k > NG_MAX_EXEC_ARGS) {
				return "too many arguments for child_exec";
			}
		}
	}

	return NULL;
}

// encodes PRE, an empty string, SUF, an empty string into BUF
static size_t exec_enc(char* buf, const char* const* pre, size_t np, const char* const* suf, size_t ns) {
	size_t at = 0;

	for(size_t k = 0; k < np; k++) {
		memcpy(buf + at, pre[k], strlen(pre[k]) + 1);
		at += strlen(pre[k]) + 1;
	}

	buf[at++] = '\0';
	for(size_t k = 0; k < ns; k++) {
		memcpy(buf + at, suf[k], strlen(suf[k]) + 1);
		at += strlen(suf[k]) + 1;
	}

	buf[at++] = '\0';

	return at;
}

static const char* exec_verdict(const char* exec, size_t len) {
	static char why[128];
	struct gb_svc sv[1] = { GB_ONESHOT("a", "print(1)") };
	struct gb_img g;
	const char* r;

	sv[0].exec = exec;
	sv[0].exec_len = len;
	g = gb_build(sv, 1, NULL, 0);
	r = ng_verify(g.map, g.len);
	snprintf(why, sizeof(why), "%s", r ? r : "");
	free(g.map);

	return r ? why : NULL;
}

static void test_verify_exec(void) {
	static const char py[] = "/usr/bin/python3\0-I\0-B\0-u\0-c\0\0a\0";
	static const char* const many[NG_MAX_EXEC_ARGS + 1] = {
		"/bin/x",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
	};
	static const char* const one[] = { "name" };
	struct gb_svc sv[1] = { { .name = "a", .type = NG_TYPE_ONESHOT, .onfail = GB_DEFAULT, .script = "print(1)", GB_EXEC(py) } };
	struct gb_img g = gb_build(sv, 1, NULL, 0);
	char* buf = malloc(2 * NG_MAX_EXEC_ARG + 64);
	char* big = malloc(NG_MAX_EXEC_ARG + 2);
	const char* p;
	const char* pre[2];
	size_t n;

	is_str(ng_verify(g.map, g.len), NULL, "a service with an interpreter argv verifies");
	ok(ng_interp(g.map, 0), "the interpreter flag is returned");
	p = ng_exec(g.map, 0);
	is_str(p, "/usr/bin/python3", "the argv starts with the interpreter");
	for(n = 0; *p; p += strlen(p) + 1) {
		n++;
	}

	is_int(n, 5, "five strings come before the script");
	is_str(p + 1, "a", "the service name comes after it");
	free(g.map);

	n = exec_enc(buf, many, NG_MAX_EXEC_ARGS, NULL, 0);
	is_str(exec_verdict(buf, n), NULL, "16 interpreter arguments verify");
	n = exec_enc(buf, many, NG_MAX_EXEC_ARGS - 1, one, 1);
	is_str(exec_verdict(buf, n), NULL, "arguments after the script count toward the 16");
	n = exec_enc(buf, many, NG_MAX_EXEC_ARGS + 1, NULL, 0);
	is_str(exec_verdict(buf, n), "too many interpreter arguments", "17 arguments are refused");
	n = exec_enc(buf, many, NG_MAX_EXEC_ARGS, one, 1);
	is_str(exec_verdict(buf, n), "too many interpreter arguments",
	       "17 counting the one after the script are refused");

	n = exec_enc(buf, (const char* const[]){ "x", "-c" }, 2, NULL, 0);
	is_str(exec_verdict(buf, n), "interpreter is not an absolute path", "a relative interpreter is refused");
	n = exec_enc(buf, (const char* const[]){ "/bin/x" }, 1, NULL, 0);
	is_str(exec_verdict(buf, n), NULL, "nothing but the interpreter verifies");
	is_str(exec_verdict("/bin/x\0-c", sizeof("/bin/x\0-c")), "interpreter arguments run past the string table",
	       "an argv without its empty strings is refused");
	is_str(exec_verdict("/bin/x\0-c\0", sizeof("/bin/x\0-c\0")),
	       "interpreter arguments run past the string table", "an argv without its second list is refused");

	big[0] = '/';
	memset(big + 1, 'p', NG_MAX_INTERP - 1);
	big[NG_MAX_INTERP] = '\0';
	pre[0] = big;
	n = exec_enc(buf, pre, 1, NULL, 0);
	is_str(exec_verdict(buf, n), NULL, "a 255 byte interpreter path verifies");
	big[NG_MAX_INTERP] = 'p';
	big[NG_MAX_INTERP + 1] = '\0';
	n = exec_enc(buf, pre, 1, NULL, 0);
	is_str(exec_verdict(buf, n), "interpreter path longer than the supported maximum",
	       "a 256 byte interpreter path is refused");

	pre[0] = "/bin/x";
	pre[1] = big;
	memset(big, 'a', NG_MAX_EXEC_ARG);
	big[NG_MAX_EXEC_ARG] = '\0';
	n = exec_enc(buf, pre, 2, NULL, 0);
	is_str(exec_verdict(buf, n), NULL, "a 4096 byte argument verifies");
	big[NG_MAX_EXEC_ARG] = 'a';
	big[NG_MAX_EXEC_ARG + 1] = '\0';
	n = exec_enc(buf, pre, 2, NULL, 0);
	is_str(exec_verdict(buf, n), "interpreter argument longer than the supported maximum",
	       "a 4097 byte argument is refused");
	free(buf);
	free(big);
}

static const char* direct_verdict(const char* exec, size_t len) {
	static char why[128];
	struct gb_svc sv[1] = { GB_DAEMON("a", "exec x") };
	struct gb_img g;
	const char* r;

	sv[0].exec = exec;
	sv[0].exec_len = len;
	sv[0].direct = 1;
	g = gb_build(sv, 1, NULL, 0);
	r = ng_verify(g.map, g.len);
	snprintf(why, sizeof(why), "%s", r ? r : "");
	free(g.map);

	return r ? why : NULL;
}

// encodes the words and an empty string into BUF
static size_t direct_enc(char* buf, const char* const* w, size_t n) {
	size_t at = 0;

	for(size_t k = 0; k < n; k++) {
		memcpy(buf + at, w[k], strlen(w[k]) + 1);
		at += strlen(w[k]) + 1;
	}

	buf[at++] = '\0';

	return at;
}

static void test_verify_direct(void) {
	static const char* const many[NG_MAX_EXEC_ARGS + 1] = {
		"agetty",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
		"-a",
	};
	struct gb_img g = gb_sample();
	char* buf = malloc(2 * NG_MAX_EXEC_ARG + 64);
	char* big = malloc(NG_MAX_EXEC_ARG + 2);
	const char* p;
	const char* w[2];
	size_t n;

	is_str(ng_verify(g.map, g.len), NULL, "a service with a direct argv verifies");
	ok(ng_direct(g.map, 5), "the direct flag is returned");
	ok(!ng_interp(g.map, 5), "and is not the interpreter flag");
	p = ng_exec(g.map, 5);
	is_str(p, "agetty", "the argv starts with the command");
	is_str(p + 7, "tty1", "its arguments follow");
	ok(!p[12], "and an empty string ends it");
	is_str(ng_script(g.map, 5), "exec agetty tty1", "the script is kept beside it");
	free(g.map);

	n = direct_enc(buf, many, NG_MAX_EXEC_ARGS);
	is_str(direct_verdict(buf, n), NULL, "16 words verify");
	n = direct_enc(buf, many, NG_MAX_EXEC_ARGS + 1);
	is_str(direct_verdict(buf, n), "too many direct arguments", "17 words are refused");
	n = direct_enc(buf, (const char* const[]){ "/bin/x" }, 1);
	is_str(direct_verdict(buf, n), NULL, "an absolute command alone verifies");
	n = direct_enc(buf, (const char* const[]){ "bin/x" }, 1);
	is_str(direct_verdict(buf, n), "command with a slash is not an absolute path",
	       "a relative command with a slash is refused");
	is_str(direct_verdict("\0", 1), "direct argv is empty", "an argv without a command is refused");
	is_str(direct_verdict("agetty\0tty1", sizeof("agetty\0tty1")), "direct arguments run past the string table",
	       "an argv without its empty string is refused");
	is_str(direct_verdict("agetty", sizeof("agetty")), "direct arguments run past the string table",
	       "a command alone without its empty string is refused");

	memset(big, 'c', NG_MAX_INTERP);
	big[NG_MAX_INTERP] = '\0';
	w[0] = big;
	n = direct_enc(buf, w, 1);
	is_str(direct_verdict(buf, n), NULL, "a 255 byte command verifies");
	big[NG_MAX_INTERP] = 'c';
	big[NG_MAX_INTERP + 1] = '\0';
	n = direct_enc(buf, w, 1);
	is_str(direct_verdict(buf, n), "command longer than the supported maximum", "a 256 byte command is refused");

	w[0] = "x";
	w[1] = big;
	memset(big, 'a', NG_MAX_EXEC_ARG);
	big[NG_MAX_EXEC_ARG] = '\0';
	n = direct_enc(buf, w, 2);
	is_str(direct_verdict(buf, n), NULL, "a 4096 byte argument verifies");
	big[NG_MAX_EXEC_ARG] = 'a';
	big[NG_MAX_EXEC_ARG + 1] = '\0';
	n = direct_enc(buf, w, 2);
	is_str(direct_verdict(buf, n), "direct argument longer than the supported maximum",
	       "a 4097 byte argument is refused");
	free(buf);
	free(big);
}

static void test_verify_fuzz(void) {
	struct gb_img base = gb_sample();
	unsigned accepted = 0, bad = 0, iters = 200000;

	for(unsigned it = 0; it < iters; it++) {
		struct gb_img g = gb_copy(&base);
		unsigned flips = 1 + tap_below(4);
		const char* why;

		for(unsigned f = 0; f < flips; f++) {
			size_t at = tap_below((uint32_t)g.len);

			if(tap_below(3)) {
				((unsigned char*)g.map)[at] ^= (unsigned char)(1u << tap_below(8));
			} else {
				((unsigned char*)g.map)[at] = (unsigned char)tap_rand();
			}
		}

		// the crc is recomputed on 7 of 8 iterations so mutations reach the structural checks
		if(tap_below(8)) {
			gb_recrc(&g);
		}

		why = ng_verify(g.map, g.len);
		if(!why) {
			const char* r = recheck(g.map, g.len);

			accepted++;
			if(r && bad++ < 5) {
				tap_diag("iteration %u accepted an image that is unsafe: %s", it, r);
			}
		}

		free(g.map);
	}

	ok(!bad, "%u mutated images, every accepted one (%u) is safe to index", iters, accepted);
	free(base.map);
}

int main(void) {
	test_verify_cases();
	test_verify_edges();
	test_verify_exec();
	test_verify_direct();
	test_verify_fuzz();

	return tap_done();
}
