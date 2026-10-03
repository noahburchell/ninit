#include "build.h"
#include "init.h"
#include "util.h"
#include "../src/ngraph.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void locale_note(void)
{
	char loc[NG_LOCALE_MAX][NG_LOCALE_LEN];
	const char *bad;
	int nl = ng_locale_env(loc, NG_LOCALE_MAX, &bad);

	fflush(stdout);
	if (bad)
		fprintf(stderr, "ninitctl: warning: %s %s\n", NG_LOCALE_CONF, bad);
	if (nl <= 0)
		fprintf(stderr,
			"ninitctl: %s %s, services will run with LANG=%s\n"
			"ninitctl: for example: printf 'LANG=en_US.UTF-8\\n' > %s\n",
			NG_LOCALE_CONF,
			access(NG_LOCALE_CONF, R_OK) ? "is missing"
						     : "sets no locale variable",
			NG_FALLBACK_LANG, NG_LOCALE_CONF);
}

int cmd_init(int argc, char **argv)
{
	const char *dir = NG_DEFAULT_DIR, *out = NULL;
	char outbuf[4096];
	struct build b = { 0 };
	const char *out_base;
	size_t out_base_len;
	int k, custom_dir = 0, dry = 0, nocheck = 0, srclock;

	// argv is already past argv[0] and the subcommand
	for (k = 0; k < argc; k++) {
		if (!strcmp(argv[k], "-d") || !strcmp(argv[k], "--dir")) {
			if (++k == argc)
				usage_die("init: %s needs a directory", argv[k - 1]);
			dir = argv[k];
			custom_dir = 1;
		} else if (!strcmp(argv[k], "-o") || !strcmp(argv[k], "--out")) {
			if (++k == argc)
				usage_die("init: %s needs a file", argv[k - 1]);
			out = argv[k];
		} else if (!strcmp(argv[k], "-n") || !strcmp(argv[k], "--dry-run")) {
			dry = 1;
		} else if (!strcmp(argv[k], "--no-check")) {
			nocheck = 1;
		} else {
			usage_die("init: unexpected argument '%s'", argv[k]);
		}
	}
	g_dir = dir;
	if (!out) {
		if (!custom_dir) {
			out = NG_DEFAULT_FILE;
		} else {
			if (snprintf(outbuf, sizeof(outbuf), "%s/depgraph", dir) >= (int)sizeof(outbuf))
				usage_die("init: directory path is too long: %s", dir);
			out = outbuf;
		}
	}

	out_base = output_base(dir, out, &out_base_len);

	srclock = lock_dir(dir);

	if (!out_base) {
		char old[4104];

		if (snprintf(old, sizeof(old), "%s.old", out) >= (int)sizeof(old))
			usage_die("init: output path is too long: %s", out);
		refuse_nongraph(out);
		refuse_nongraph(old);
	}

	b.dir = dir;
	read_sources(&b, out_base, out_base_len);
	if (!nocheck)
		check_syntax(b.srcs, b.n, dir);
	link_graph(&b);
	order_graph(&b);
	write_image(&b, out, dry, srclock);
	locale_note();

	return 0;
}
