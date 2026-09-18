#include "add.h"
#include "build.h"
#include "ctl.h"
#include "del.h"
#include "show.h"

#include "../src/ngraph.h"
#include <stdio.h>
#include <string.h>

static void usage(FILE *f)
{
	fputs("Usage: ninitctl COMMAND [OPTION]...\n"
	      "\n"
	      "Configuration commands:\n"
	      "  init [-d DIR] [-o FILE] [-n] [--no-check]\n"
	      "                              compile DIR into a depgraph\n"
	      "  show [-f FILE] [-v]         print the compiled depgraph\n"
	      "  add [-d DIR] [--] NAME...   move services out of DIR/unused\n"
	      "  del [-d DIR] [--] NAME...   move services into DIR/unused\n"
	      "\n"
	      "Runtime commands (via " NINIT_CTL_SOCK ", root only):\n"
	      "  status [NAME]               report service state\n"
	      "  start NAME                  start a service\n"
	      "  stop NAME                   stop a service\n"
	      "  restart NAME                stop, then start a service\n"
	      "  resume                      retry failed and skipped services\n"
	      "\n"
	      "DIR defaults to " NG_DEFAULT_DIR ", FILE to " NG_DEFAULT_FILE ".\n"
	      "A rebuilt depgraph takes effect on the next boot.\n",
	      f);
}

static int takes_value(const char *a)
{
	return !strcmp(a, "-d") || !strcmp(a, "--dir") ||
	       !strcmp(a, "-o") || !strcmp(a, "--out") ||
	       !strcmp(a, "-f") || !strcmp(a, "--file");
}

static int wants_help(int argc, char **argv)
{
	int k;

	for (k = 0; k < argc; k++) {
		if (!strcmp(argv[k], "--"))
			return 0;
		if (takes_value(argv[k])) {
			k++;
			continue;
		}
		if (!strcmp(argv[k], "-h") || !strcmp(argv[k], "--help"))
			return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const char *const ctl_verbs[] = { "status", "log", "start", "stop",
						 "restart", "resume", "reload" };
	int rest = argc - 2;
	char **args = argv + 2;
	size_t v;

	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
		usage(stdout);
		return 0;
	}

	if (wants_help(rest, args)) {
		usage(stdout);
		return 0;
	}

	if (!strcmp(argv[1], "init"))
		return cmd_init(rest, args);
	if (!strcmp(argv[1], "show"))
		return cmd_show(rest, args);
	if (!strcmp(argv[1], "add"))
		return cmd_add(rest, args);
	if (!strcmp(argv[1], "del"))
		return cmd_del(rest, args);
	for (v = 0; v < sizeof(ctl_verbs) / sizeof(*ctl_verbs); v++)
		if (!strcmp(argv[1], ctl_verbs[v]))
			return cmd_ctl(ctl_verbs[v], rest, args);

	fprintf(stderr, "ninitctl: unknown command '%s'\n", argv[1]);
	usage(stderr);
	return 2;
}
