#include "add.h"
#include "build.h"
#include "del.h"
#include "show.h"

#include "../src/ngraph.h"
#include <stdio.h>
#include <string.h>

static void usage(FILE *f)
{
	fputs("usage: ninitctl <command> [options]\n"
	      "\n"
	      "  init [-d DIR] [-o FILE]   compile DIR into a depgraph\n"
	      "  show [-f FILE] [-v]       print the compiled depgraph\n"
	      "  add  [-d DIR] NAME...     move services out of DIR/unused\n"
	      "  del  [-d DIR] NAME...     move services into DIR/unused\n"
	      "\n"
	      "DIR defaults to " NG_DEFAULT_DIR ", FILE to " NG_DEFAULT_FILE ".\n",
	      f);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	if (!strcmp(argv[1], "init"))
		return cmd_init(argc - 2, argv + 2);
	if (!strcmp(argv[1], "show"))
		return cmd_show(argc - 2, argv + 2);
	if (!strcmp(argv[1], "add"))
		return cmd_add(argc - 2, argv + 2);
	if (!strcmp(argv[1], "del"))
		return cmd_del(argc - 2, argv + 2);
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
		usage(stdout);
		return 0;
	}

	fprintf(stderr, "ninitctl: unknown command '%s'\n", argv[1]);
	usage(stderr);
	return 2;
}
