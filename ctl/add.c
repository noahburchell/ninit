#include "add.h"
#include "util.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <sys/file.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SVC_UNUSED "unused"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif

static int rename_noreplace(const char* from, const char* to) {
#ifdef HAVE_RENAMEAT2
	return renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE);
#else
	return (int)syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to,
			    (uint32_t)RENAME_NOREPLACE);
#endif
}

static void print_rebuild_hint(const char* dir, bool custom) {
	const char* p;

	if(!custom) {
		printf("run: ninitctl init\n");
		return;
	}

	fputs("run: ninitctl init -d '", stdout);
	for(p = dir; *p; p++) {
		if(*p == '\'') {
			fputs("'\\''", stdout);
		} else {
			putchar(*p);
		}
	}

	fputs("'\n", stdout);
}

static bool join(char* out, size_t cap, const char* dir, const char* name) {
	size_t dl = strlen(dir), nl = strlen(name);

	if(dl + nl + 2 > cap) {
		return false;
	}

	memcpy(out, dir, dl);
	out[dl] = '/';
	memcpy(out + dl + 1, name, nl + 1);

	return true;
}

int svc_move(int argc, char** argv, bool enable) {
	const char* dir = NG_DEFAULT_DIR;
	const char* verb = enable ? "add" : "del";
	char unused[4096], from[4096], to[4096];
	int k, dfd, endopts = 0;
	uint32_t names = 0, moved = 0, bad = 0;
	bool custom_dir = false;

	setvbuf(stdout, NULL, _IOLBF, 0);

	for(k = 0; k < argc; k++) {
		if(endopts) {
			names++;
		} else if(!strcmp(argv[k], "--")) {
			endopts = k + 1;
		} else if(!strcmp(argv[k], "-d") || !strcmp(argv[k], "--dir")) {
			if(++k == argc) {
				usage_die("%s: %s needs a directory", verb, argv[k - 1]);
			}

			dir = argv[k];
			custom_dir = true;
		} else if(argv[k][0] == '-') {
			usage_die("%s: unknown option '%s'", verb, argv[k]);
		} else {
			names++;
		}
	}

	if(!names) {
		usage_die("%s needs at least one service name", verb);
	}

	if(!join(unused, sizeof(unused), dir, SVC_UNUSED)) {
		usage_die("%s: directory path is too long", verb);
	}

	if(!enable && mkdir(unused, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "ninitctl: mkdir %s: %s\n", unused, strerror(errno));
		return 1;
	}

	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if(dfd < 0) {
		fprintf(stderr, "ninitctl: open %s: %s\n", dir, strerror(errno));
		return 1;
	}

	if(flock(dfd, LOCK_EX) < 0) {
		fprintf(stderr, "ninitctl: lock %s: %s\n", dir, strerror(errno));
		close(dfd);
		return 1;
	}

	for(k = 0; k < argc; k++) {
		const char* name = argv[k];
		const char* why;
		struct stat st, lst;
		char target[4096];

		if(!endopts || k < endopts) {
			if(!strcmp(name, "--")) {
				continue;
			}

			if(!strcmp(name, "-d") || !strcmp(name, "--dir")) {
				k++;
				continue;
			}
		}

		why = ng_name_problem(name);
		if(why) {
			fprintf(stderr, "ninitctl: '%s' %s\n", name, why);
			bad++;
			continue;
		}

		if(ng_reserved_name(name)) {
			fprintf(stderr, "ninitctl: '%s' is a reserved name\n", name);
			bad++;
			continue;
		}

		if(!join(from, sizeof(from), enable ? unused : dir, name)
		   || !join(to, sizeof(to), enable ? dir : unused, name)) {
			fprintf(stderr, "ninitctl: %s: path is too long\n", name);
			bad++;
			continue;
		}

		// symlinks are followed, a dangling one is named for what it is
		if(stat(from, &st) < 0) {
			if(lstat(from, &lst) == 0 && S_ISLNK(lst.st_mode)) {
				ssize_t ln = readlink(from, target, sizeof(target) - 1);

				target[ln > 0 ? ln : 0] = '\0';
				fprintf(stderr, "ninitctl: %s: symlink target %s is missing\n", name, target);
			} else {
				fprintf(stderr, "ninitctl: %s: not in %s\n", name, enable ? unused : dir);
			}

			bad++;
			continue;
		}

		if(!S_ISREG(st.st_mode)) {
			fprintf(stderr, "ninitctl: %s: is a %s\n", name,
				S_ISDIR(st.st_mode) ? "directory" : "special file");
			bad++;
			continue;
		}

		if(lstat(from, &lst) == 0 && S_ISLNK(lst.st_mode)) {
			ssize_t ln = readlink(from, target, sizeof(target) - 1);

			if(ln < 0) {
				fprintf(stderr, "ninitctl: readlink %s: %s\n", name,
					strerror(errno));
				bad++;
				continue;
			}

			target[ln] = '\0';
			// from the other directory it would point somewhere else
			if(target[0] != '/') {
				fprintf(stderr, "ninitctl: %s: symlink target %s is relative, use an absolute target\n", name, target);
				bad++;
				continue;
			}
		}

		if(rename_noreplace(from, to) < 0) {
			if(errno == ENOENT) {
				fprintf(stderr, "ninitctl: %s: not in %s\n", name,
					enable ? unused : dir);
			} else if(errno == EEXIST) {
				fprintf(stderr, "ninitctl: %s: already in %s\n", name,
					enable ? dir : unused);
			} else {
				fprintf(stderr, "ninitctl: %s %s: %s\n", verb, name,
					strerror(errno));
			}

			bad++;
			continue;
		}

		printf("%s %s\n", enable ? "added" : "removed", name);
		moved++;
	}

	if(moved) {
		int ufd = open(unused, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

		if(fsync(dfd) < 0) {
			fprintf(stderr, "ninitctl: sync %s: %s\n", dir, strerror(errno));
		}

		if(ufd >= 0) {
			if(fsync(ufd) < 0) {
				fprintf(stderr, "ninitctl: sync %s: %s\n", unused,
					strerror(errno));
			}

			close(ufd);
		}
	}

	close(dfd);

	if(moved) {
		print_rebuild_hint(dir, custom_dir);
	}

	return bad ? 1 : 0;
}

int cmd_add(int argc, char** argv) {
	return svc_move(argc, argv, true);
}
