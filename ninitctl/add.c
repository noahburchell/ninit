#include "add.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <sys/file.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SVC_UNUSED	"unused"

static void print_rebuild_hint(const char *dir, int custom)
{
	const char *p;

	if (!custom) {
		printf("rebuild the depgraph with:  ninitctl init\n");
		return;
	}
	fputs("rebuild the depgraph with:  ninitctl init -d '", stdout);
	for (p = dir; *p; p++) {
		if (*p == '\'')
			fputs("'\\''", stdout);
		else
			putchar(*p);
	}
	fputs("'\n", stdout);
}

static int relink(const char *path, const char *target, int enable)
{
	char fixed[4200], tmp[4200];
	int n;

	if (enable) {
		if (!strncmp(target, "../", 3))
			n = snprintf(fixed, sizeof(fixed), "%s", target + 3);
		else
			n = snprintf(fixed, sizeof(fixed), "%s/%s", SVC_UNUSED, target);
	} else {
		n = snprintf(fixed, sizeof(fixed), "../%s", target);
	}
	if (n < 0 || (size_t)n >= sizeof(fixed))
		return 0;
	if (!strcmp(fixed, target))
		return 1;

	for (unsigned t = 0; ; t++) {
		n = snprintf(tmp, sizeof(tmp), "%s.ninitctl.%d.%u.tmp", path,
			     (int)getpid(), t);
		if (n < 0 || (size_t)n >= sizeof(tmp))
			return 0;
		if (symlink(fixed, tmp) == 0)
			break;
		if (errno != EEXIST || t == 999)
			return 0;
	}
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return 0;
	}
	return 1;
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
	int k, names = 0, moved = 0, bad = 0, dfd, endopts = 0, custom_dir = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);

	for (k = 0; k < argc; k++) {
		if (endopts) {
			names++;
		} else if (!strcmp(argv[k], "--")) {
			endopts = k + 1;
		} else if (!strcmp(argv[k], "-d") || !strcmp(argv[k], "--dir")) {
			if (++k == argc) {
				fprintf(stderr, "ninitctl: %s: %s needs a directory\n",
					verb, argv[k - 1]);
				return 2;
			}
			dir = argv[k];
			custom_dir = 1;
		} else if (argv[k][0] == '-') {
			fprintf(stderr, "ninitctl: %s: unknown option '%s' "
				"(use -- before a name that starts with '-')\n", verb, argv[k]);
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

	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0) {
		fprintf(stderr, "ninitctl: open %s: %s\n", dir, strerror(errno));
		return 1;
	}
	if (flock(dfd, LOCK_EX) < 0) {
		fprintf(stderr, "ninitctl: lock %s: %s\n", dir, strerror(errno));
		close(dfd);
		return 1;
	}

	for (k = 0; k < argc; k++) {
		const char *name = argv[k], *why;
		struct stat st, lst;
		char target[4096];
		size_t link_len;

		if (!endopts || k < endopts) {
			if (!strcmp(name, "--"))
				continue;
			if (!strcmp(name, "-d") || !strcmp(name, "--dir")) {
				k++;
				continue;
			}
		}

		why = ng_name_problem(name);
		if (why) {
			fprintf(stderr, "ninitctl: '%s' %s\n", name, why);
			bad++;
			continue;
		}
		if (ng_reserved_name(name)) {
			fprintf(stderr, "ninitctl: '%s' is a ninitctl artifact, not a service\n",
				name);
			bad++;
			continue;
		}
		if (!join(from, sizeof(from), enable ? unused : dir, name) ||
		    !join(to, sizeof(to), enable ? dir : unused, name)) {
			fprintf(stderr, "ninitctl: %s: path is too long\n", name);
			bad++;
			continue;
		}

		// symlinks are followed
		if (stat(from, &st) < 0) {
			fprintf(stderr, "ninitctl: %s: not in %s\n", name,
				enable ? unused : dir);
			bad++;
			continue;
		}
		if (!S_ISREG(st.st_mode)) {
			fprintf(stderr, "ninitctl: %s: is a %s, not a service\n", name,
				S_ISDIR(st.st_mode) ? "directory" : "special file");
			bad++;
			continue;
		}

		link_len = 0;
		if (lstat(from, &lst) == 0 && S_ISLNK(lst.st_mode)) {
			ssize_t ln = readlink(from, target, sizeof(target) - 1);

			if (ln < 0) {
				fprintf(stderr, "ninitctl: readlink %s: %s\n", name,
					strerror(errno));
				bad++;
				continue;
			}
			target[ln] = '\0';
			if (target[0] != '/')
				link_len = (size_t)ln;
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
		if (link_len && !relink(to, target, enable)) {
			fprintf(stderr, "ninitctl: %s: moved, but its relative link "
				"could not be repointed\n", name);
			bad++;
		}

		printf("%s %s\n", enable ? "added" : "removed", name);
		moved++;
	}

	if (moved) {
		int ufd = open(unused, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

		if (fsync(dfd) < 0)
			fprintf(stderr, "ninitctl: sync %s: %s; the move may not "
				"survive a crash\n", dir, strerror(errno));
		if (ufd >= 0) {
			if (fsync(ufd) < 0)
				fprintf(stderr, "ninitctl: sync %s: %s; the move may not "
					"survive a crash\n", unused, strerror(errno));
			close(ufd);
		}
	}
	close(dfd);

	if (moved)
		print_rebuild_hint(dir, custom_dir);

	return bad ? 1 : 0;
}

int cmd_add(int argc, char **argv)
{
	return svc_move(argc, argv, 1);
}
