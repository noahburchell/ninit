#include "add.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SVC_UNUSED	"unused"

static int bad_name(const char *n)
{
	return !*n || n[0] == '.' || strchr(n, '/') != NULL;
}

static int join(char *out, size_t cap, const char *dir, const char *name)
{
	size_t dl = strlen(dir), nl = strlen(name);

	if (dl + nl + 2 > cap)
		return 0;
	memcpy(out, dir, dl);
	out[dl] = '/';
	memcpy(out + dl + 1, name, nl + 1);
	return 1;
}

int svc_move(int argc, char **argv, int enable)
{
	const char *dir = NG_DEFAULT_DIR;
	const char *verb = enable ? "add" : "del";
	char unused[4096], from[4096], to[4096];
	int k, names = 0, moved = 0, bad = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);

	for (k = 0; k < argc; k++) {
		if (!strcmp(argv[k], "-d") || !strcmp(argv[k], "--dir")) {
			if (++k == argc) {
				fprintf(stderr, "ninitctl: %s: %s needs a directory\n",
					verb, argv[k - 1]);
				return 2;
			}
			dir = argv[k];
		} else if (argv[k][0] == '-') {
			fprintf(stderr, "ninitctl: %s: unknown option '%s'\n", verb, argv[k]);
			return 2;
		} else {
			names++;
		}
	}
	if (!names) {
		fprintf(stderr, "ninitctl: %s needs at least one service name\n", verb);
		return 2;
	}

	if (!join(unused, sizeof(unused), dir, SVC_UNUSED)) {
		fprintf(stderr, "ninitctl: %s: directory path is too long\n", verb);
		return 2;
	}
	if (!enable && mkdir(unused, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "ninitctl: mkdir %s: %s\n", unused, strerror(errno));
		return 1;
	}

	for (k = 0; k < argc; k++) {
		const char *name = argv[k];

		if (!strcmp(name, "-d") || !strcmp(name, "--dir")) {
			k++;
			continue;
		}
		if (bad_name(name)) {
			fprintf(stderr, "ninitctl: '%s' is not a service name\n", name);
			bad++;
			continue;
		}
		if (!join(from, sizeof(from), enable ? unused : dir, name) ||
		    !join(to, sizeof(to), enable ? dir : unused, name)) {
			fprintf(stderr, "ninitctl: %s: path is too long\n", name);
			bad++;
			continue;
		}

		if (renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) < 0) {
			if (errno == ENOENT)
				fprintf(stderr, "ninitctl: %s: not in %s\n", name,
					enable ? unused : dir);
			else if (errno == EEXIST)
				fprintf(stderr, "ninitctl: %s: already in %s\n", name,
					enable ? dir : unused);
			else
				fprintf(stderr, "ninitctl: %s %s: %s\n", verb, name,
					strerror(errno));
			bad++;
			continue;
		}
		printf("%s %s\n", enable ? "added" : "removed", name);
		moved++;
	}

	if (moved)
		printf("run 'ninitctl init' to rebuild the depgraph\n");

	return bad ? 1 : 0;
}

int cmd_add(int argc, char **argv)
{
	return svc_move(argc, argv, 1);
}
