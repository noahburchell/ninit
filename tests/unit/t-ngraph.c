#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tap.h"

// ng_locale_env reads a fixed path, so it is pointed at a scratch file here
static const char* locale_path;

static int shim_open(const char* path, int flags, ...) {
	if(!strcmp(path, "/etc/locale.conf")) {
		if(!locale_path) {
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

static void test_crc(void) {
	static const struct {
		const char* in;
		size_t len;
		uint32_t crc;
	} v[] = {
		{ "", 0, 0x00000000u },
		{ "a", 1, 0xc1d04330u },
		{ "123456789", 9, 0xe3069283u },
		{ "The quick brown fox jumps over the lazy dog", 43, 0x22620404u },
	};
	unsigned char zeros[32] = { 0 }, ones[32], inc[32], dec[32];

	for(size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		ok(ng_crc32c(v[k].in, v[k].len) == v[k].crc, "crc32c of \"%s\"", v[k].in);
	}

	// rfc 3720 b.4
	memset(ones, 0xff, sizeof(ones));
	for(int k = 0; k < 32; k++) {
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

		for(size_t k = 0; k < sizeof(buf); k++) {
			buf[k] = (unsigned char)tap_rand();
		}

		for(size_t at = 0; at < 9; at++) {
			for(size_t len = 0; len + at <= sizeof(buf); len += 1 + len / 7) {
				uint32_t sw = ~(uint32_t)crc32c_feed_sw(~0u, buf + at, len);

				if(sw != ng_crc32c(buf + at, len) && bad++ < 5) {
					tap_diag("mismatch at offset %zu length %zu", at, len);
				}
			}
		}

		ok(!bad, "crc32c dispatcher matches the table implementation");
	}

	// incremental feeding must equal a single pass
	{
		const char* s = "split anywhere and the running crc must not care";
		size_t n = strlen(s);
		int bad = 0;

		for(size_t cut = 0; cut <= n; cut++) {
			uint64_t c = crc32c_feed(~0u, s, cut);

			c = crc32c_feed(c, s + cut, n - cut);
			bad += ~(uint32_t)c != ng_crc32c(s, n);
		}

		ok(!bad, "crc32c is the same however the input is split");
	}
}

static void test_image_crc(void) {
	struct gb_img g = gb_sample();
	struct ng_hdr* h = gb_hdr(&g);
	uint32_t c = h->crc32;

	is_int(ng_image_crc32c(g.map, g.len), c, "image crc skips its own field");
	h->crc32 = 0xdeadbeef;
	is_int(ng_image_crc32c(g.map, g.len), c, "image crc ignores what is stored in the field");
	h->crc32 = 0;
	is_int(ng_crc32c(g.map, g.len), c, "image crc is the crc of the image with the field zeroed");
	is_int(ng_image_crc32c(g.map, 63), 0, "a buffer shorter than the header has crc 0");
	free(g.map);
}

static void test_names(void) {
	static const struct {
		const char* name;
		const char* why;
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
		{ "a\x01"
		  "b",
		  "contains whitespace or a control character" },
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

	for(size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
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

static void test_accessors(void) {
	static const struct gb_svc sv[] = {
		GB_ONESHOT("a", "true"),
		GB_DAEMON("b", "exec sleep 1"),
		{ .name = "c", .type = NG_TYPE_DAEMON, .onfail = NG_ONFAIL_SHELL, .restart = 1, .notify = 7, .script = "x", .pol = { .start_ms = 1, .stop_ms = 2, .retry_ms = 3, .start_tries = 4, .pflags = NG_PF_ORDER_ONLY } },
	};
	struct gb_img g = gb_build(sv, 3, NULL, 0);
	const void* m = g.map;

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

static char locale_tmp[] = "/tmp/ninit-t-ngraph-locale.XXXXXX";

static void put_locale(const char* text, size_t len) {
	int fd = open(locale_tmp, O_WRONLY | O_TRUNC | O_CLOEXEC);

	if(fd < 0 || write(fd, text, len) != (ssize_t)len) {
		abort();
	}

	close(fd);
}

static int run_locale(const char* text, char (*out)[NG_LOCALE_LEN], int max, const char** why) {
	put_locale(text, strlen(text));
	locale_path = locale_tmp;
	memset(out, 0, (size_t)NG_LOCALE_MAX * NG_LOCALE_LEN);

	return ng_locale_env(out, max, why);
}

static void test_locale(void) {
	char out[NG_LOCALE_MAX][NG_LOCALE_LEN];
	const char* why;
	int n, fd;

	fd = mkstemp(locale_tmp);
	if(fd < 0) {
		abort();
	}

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
		static const char* const all[] = {
			"LANG",
			"LC_ALL",
			"LC_CTYPE",
			"LC_NUMERIC",
			"LC_TIME",
			"LC_COLLATE",
			"LC_MONETARY",
			"LC_MESSAGES",
			"LC_PAPER",
			"LC_NAME",
			"LC_ADDRESS",
			"LC_TELEPHONE",
			"LC_MEASUREMENT",
			"LC_IDENTIFICATION",
		};
		char text[1024];
		size_t at = 0;

		for(size_t k = 0; k < 14; k++) {
			at += (size_t)snprintf(text + at, sizeof(text) - at, "%s=C\n", all[k]);
		}

		n = run_locale(text, out, NG_LOCALE_MAX, &why);
		is_int(n, 14, "all 14 locale variables are carried");
		is_str(out[13], "LC_IDENTIFICATION=C", "the last of them too");
	}

	n = run_locale("LANG=C", out, NG_LOCALE_MAX, &why);
	is_int(n, 1, "a last line without a newline counts");

	{
		char* big = malloc(9000);

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

int main(void) {
	test_crc();
	test_image_crc();
	test_names();
	test_accessors();
	test_locale();

	return tap_done();
}
