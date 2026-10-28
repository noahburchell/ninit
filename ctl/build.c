#include "build.h"
#include "init.h"
#include "util.h"
#include "parser/parser.h"
#include "../src/ngraph.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void locale_note(void) {
	char loc[NG_LOCALE_MAX][NG_LOCALE_LEN];
	const char* bad;
	int fd = sys_open(NG_LOCALE_CONF, O_RDONLY | O_NOCTTY);
	int nl = ng_locale_fd(fd, loc, NG_LOCALE_MAX, &bad);

	fflush(stdout);
	if(bad) {
		fprintf(stderr, "ninitctl: warning: %s%s %s\n", g_root_pfx, NG_LOCALE_CONF, bad);
	}

	if(nl <= 0) {
		fprintf(stderr,
			"ninitctl: %s%s %s, services will run with LANG=%s\n"
			"ninitctl: for example: printf 'LANG=en_US.UTF-8\\n' > %s%s\n",
			g_root_pfx, NG_LOCALE_CONF, fd < 0 ? "is missing" : "sets no locale variable",
			NG_FALLBACK_LANG, g_root_pfx, NG_LOCALE_CONF);
	}
}

// an interpreter on a filesystem of its own is there at boot only once something has mounted it
static void interp_fs_note(const struct build* b) {
	struct stat root, st;

	if(sys_stat("/", &root) < 0) {
		return;
	}

	for(uint32_t i = 0; i < b->n; i++) {
		const struct src* s = &b->srcs[i];

		if(!s->script || s->lang == &lang_shell || b->g.indeg[i]) {
			continue;
		}

		if(sys_stat(s->interp, &st) == 0 && st.st_dev != root.st_dev) {
			fprintf(stderr, "ninitctl: warning: %s/%s: %s%s is on another filesystem than %s/, "
					"and the service has no prerequisite\n",
				g_dir, s->name, g_root_pfx, s->interp,
				g_root_pfx);
		}
	}
}

int cmd_init(int argc, char** argv) {
	const char* dir = NG_DEFAULT_DIR;
	const char* out = NULL;
	char outbuf[4096];
	struct build b = { 0 };
	str out_base;
	int k, custom_dir = 0, dry = 0, nocheck = 0, srclock;

	// argv is already past argv[0] and the subcommand
	for(k = 0; k < argc; k++) {
		if(!strcmp(argv[k], "-d") || !strcmp(argv[k], "--dir")) {
			if(++k == argc) {
				usage_die("init: %s needs a directory", argv[k - 1]);
			}

			dir = argv[k];
			custom_dir = 1;
		} else if(!strcmp(argv[k], "-o") || !strcmp(argv[k], "--out")) {
			if(++k == argc) {
				usage_die("init: %s needs a file", argv[k - 1]);
			}

			out = argv[k];
		} else if(!strcmp(argv[k], "-n") || !strcmp(argv[k], "--dry-run")) {
			dry = 1;
		} else if(!strcmp(argv[k], "--no-check")) {
			nocheck = 1;
		} else if(!strcmp(argv[k], "--root")) {
			if(++k == argc) {
				usage_die("init: %s needs a directory", argv[k - 1]);
			}

			set_root(argv[k]);
		} else {
			usage_die("init: unexpected argument '%s'", argv[k]);
		}
	}

	if(g_root && !custom_dir) {
		size_t pl = strlen(g_root_pfx);
		char* rd = xmalloc(pl + sizeof(NG_DEFAULT_DIR));

		memcpy(rd, g_root_pfx, pl);
		memcpy(rd + pl, NG_DEFAULT_DIR, sizeof(NG_DEFAULT_DIR));
		dir = rd;
		custom_dir = 1;
	}

	g_dir = dir;
	g_sh_lexed = sh_lexed(NG_SHELL_ARGV0);
	if(!out) {
		if(!custom_dir) {
			out = NG_DEFAULT_FILE;
		} else {
			if(snprintf(outbuf, sizeof(outbuf), "%s/depgraph", dir) >= (int)sizeof(outbuf)) {
				usage_die("init: directory path is too long: %s", dir);
			}

			out = outbuf;
		}
	}

	out_base = output_base(dir, out);

	srclock = lock_dir(dir);

	if(!out_base.p) {
		char old[4104];

		if(snprintf(old, sizeof(old), "%s.old", out) >= (int)sizeof(old)) {
			usage_die("init: output path is too long: %s", out);
		}

		refuse_nongraph(out);
		refuse_nongraph(old);
	}

	b.dir = dir;
	read_sources(&b, out_base);
	if(!nocheck) {
		check_syntax(b.srcs, b.n, dir);
	}

	link_graph(&b);
	order_graph(&b);
	write_image(&b, out, dry, srclock);
	fflush(stdout);
	interp_fs_note(&b);
	locale_note();

	return 0;
}
