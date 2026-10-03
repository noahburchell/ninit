#include "init.h"
#include "util.h"
#include "parser/parser.h"
#include "../src/ngraph.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int keep(const struct dirent *d)
{
	return d->d_name[0] != '.';
}

static int is_artifact(const char *name, const char *base, size_t base_len)
{
	if (!base || strncmp(name, base, base_len))
		return 0;
	return !name[base_len] || !strcmp(name + base_len, ".old") ||
	       !strcmp(name + base_len, ".tmp");
}

static int cmp_dirent(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

void read_sources(struct build *b, const char *out_base, size_t out_base_len)
{
	const char *dir = b->dir, *why;
	struct dirent **ents;
	struct src *srcs;
	uint64_t hash = 0xcbf29ce484222325ull;
	mode_t out_mode = 0644;
	uint32_t n;
	int ne, k;

	ne = scandir(dir, &ents, keep, cmp_dirent);
	if (ne < 0)
		die("scandir %s: %s", dir, strerror(errno));
	if ((uint32_t)ne > NG_MAX_SVC + 4)
		die("%d entries in %s, the maximum is %u", ne, dir, NG_MAX_SVC);

	srcs = xmalloc((size_t)ne * sizeof(*srcs));
	n = 0;
	for (k = 0; k < ne; k++) {
		char path[4096];
		struct stat st;
		char *buf;
		size_t len;
		mode_t sm;

		int is_out = is_artifact(ents[k]->d_name, out_base, out_base_len);
		int is_dg = is_artifact(ents[k]->d_name, "depgraph", 8) ||
			    ng_reserved_name(ents[k]->d_name);

		if (snprintf(path, sizeof(path), "%s/%s", dir, ents[k]->d_name) >= (int)sizeof(path))
			die("%s/%s: path is too long", dir, ents[k]->d_name);
		if (stat(path, &st) < 0)
			die("stat %s: %s", path, strerror(errno));

		if (is_out || is_dg) {
			if (!S_ISREG(st.st_mode) || !graph_image(path)) {
				// the build replaces the output and its .old, never its .tmp
				if (is_out && strcmp(ents[k]->d_name + out_base_len, ".tmp"))
					die("%s/%s: the output would replace this, and it is not "
					    "a depgraph", dir, ents[k]->d_name);
				if (!S_ISDIR(st.st_mode))
					fprintf(stderr, "ninitctl: warning: %s/%s has a reserved name, "
						"skipping it\n", dir, ents[k]->d_name);
			}
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;

		why = ng_name_problem(ents[k]->d_name);
		if (why)
			die("%s/%s: filename %s", dir, ents[k]->d_name, why);

		sm = st.st_mode;
		if (!(sm & S_IRGRP))
			out_mode &= (mode_t)~(S_IRGRP | S_IWGRP);
		if (!(sm & S_IROTH))
			out_mode &= (mode_t)~(S_IROTH | S_IWOTH);

		if ((uint64_t)st.st_size > NG_MAX_SRC && graph_image(path)) {
			fprintf(stderr, "ninitctl: warning: %s is a compiled depgraph, "
				"skipping it\n", path);
			continue;
		}

		buf = slurp(path, &len);

		// a graph left in the services directory under any other name
		if (len >= sizeof(struct ng_hdr) &&
		    ((const struct ng_hdr *)(const void *)buf)->magic == NG_MAGIC &&
		    ((const struct ng_hdr *)(const void *)buf)->total_len == len) {
			fprintf(stderr, "ninitctl: warning: %s is a compiled depgraph, "
				"skipping it\n", path);
			free(buf);
			continue;
		}

		for (const char *p = ents[k]->d_name; *p; p++)
			hash = (hash ^ (unsigned char)*p) * 0x100000001b3ull;
		hash = (hash ^ 0) * 0x100000001b3ull;
		for (size_t q = 0; q < len; q++)
			hash = (hash ^ (unsigned char)buf[q]) * 0x100000001b3ull;
		hash = (hash ^ 0) * 0x100000001b3ull;

		parse_src(&srcs[n++], ents[k]->d_name, buf, len);
	}
	if (!n)
		die("no services in %s", dir);
	if (n > NG_MAX_SVC)
		die("%u services in %s, the maximum is %u", n, dir, NG_MAX_SVC);

	b->srcs = srcs;
	b->n = n;
	b->hash = hash;
	b->out_mode = out_mode;
}
