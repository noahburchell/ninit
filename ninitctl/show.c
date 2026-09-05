#include "show.h"
#include "../src/ngraph.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

struct style {
	const char *bold, *dim, *cyan, *yellow, *green, *red, *reset;
	const char *bar, *vbar, *arrow, *none;
};

static const struct style style_tty = {
	"\033[1m", "\033[2m", "\033[36m", "\033[33m", "\033[32m", "\033[1;31m", "\033[0m",
	"─", "│", "→", "—",
};

static const struct style style_plain = {
	"", "", "", "", "", "", "",
	"-", "|", "->", "-",
};

int cmd_show(int argc, char **argv)
{
	const char *path = NG_DEFAULT_FILE, *why;
	const struct style *st;
	const struct ng_hdr *h;
	const struct ng_svc *sv;
	const uint32_t *roff, *ridx;
	uint32_t *doff, *didx, *fill, *level;
	uint32_t n, m, i, j, wname = 7, maxlvl = 0, deepest = 0;
	struct stat sb;
	void *map;
	int fd, k, verbose = 0;

	for (k = 0; k < argc; k++) {
		if ((!strcmp(argv[k], "-f") || !strcmp(argv[k], "--file")) && k + 1 < argc)
			path = argv[++k];
		else if (!strcmp(argv[k], "-v") || !strcmp(argv[k], "--verbose"))
			verbose = 1;
		else if (argv[k][0] != '-')
			path = argv[k];
		else {
			fprintf(stderr, "ninitctl: show: unexpected argument '%s'\n", argv[k]);
			return 1;
		}
	}

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "ninitctl: open %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (fstat(fd, &sb) < 0 || sb.st_size <= 0) {
		fprintf(stderr, "ninitctl: %s: not a usable file\n", path);
		return 1;
	}
	map = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		fprintf(stderr, "ninitctl: mmap %s: %s\n", path, strerror(errno));
		return 1;
	}

	why = ng_verify(map, (size_t)sb.st_size);
	if (why) {
		fprintf(stderr, "ninitctl: %s: %s\n", path, why);
		return 1;
	}

	st = isatty(STDOUT_FILENO) ? &style_tty : &style_plain;

	h = map;
	sv = ng_svcs(map);
	roff = ng_rdep_off(map);
	ridx = ng_rdep_idx(map);
	n = h->n_svc;
	m = h->n_edges;

	doff = calloc(n + 2, sizeof(*doff));
	didx = calloc(m ? m : 1, sizeof(*didx));
	fill = calloc(n + 1, sizeof(*fill));
	level = calloc(n, sizeof(*level));
	if (!doff || !didx || !fill || !level) {
		fprintf(stderr, "ninitctl: out of memory\n");
		return 1;
	}
	for (i = 0; i < m; i++)
		doff[ridx[i] + 1]++;
	for (i = 0; i < n; i++)
		doff[i + 1] += doff[i];
	for (i = 0; i < n; i++)
		for (j = roff[i]; j < roff[i + 1]; j++)
			didx[doff[ridx[j]] + fill[ridx[j]]++] = i;

	for (i = 0; i < n; i++) {
		uint32_t lv = 1;

		for (j = doff[i]; j < doff[i + 1]; j++)
			if (level[didx[j]] + 1 > lv)
				lv = level[didx[j]] + 1;
		level[i] = lv;
		if (lv > maxlvl) {
			maxlvl = lv;
			deepest = i;
		}
		if (strlen(ng_name(map, i)) > wname)
			wname = (uint32_t)strlen(ng_name(map, i));
	}

	printf("%s%sdepgraph%s  %s\n", st->bold, st->cyan, st->reset, path);
	printf("%s%u services, %u edges, %u roots, %u levels, %lu bytes, crc %08x%s\n\n",
	       st->dim, n, m, h->n_roots, maxlvl, (unsigned long)sb.st_size, h->crc32, st->reset);

	printf("%s  #  %-*s  %-7s  lvl  kills  onfail  rdy  rst  depends on%s\n",
	       st->dim, (int)wname, "service", "type", st->reset);
	fputs(st->dim, stdout);
	fputs("  ", stdout);
	for (i = 0; i < wname + 59; i++)
		fputs(st->bar, stdout);
	printf("%s\n", st->reset);

	for (i = 0; i < n; i++) {
		const char *col = i < h->n_roots ? st->green : "";
		const char *cend = i < h->n_roots ? st->reset : "";
		uint8_t pol = ng_onfail(map, i);
		const char *pcol = pol == NG_ONFAIL_SHELL ? st->red :
				   pol == NG_ONFAIL_STOP ? st->yellow : st->dim;

		char rdy[8];

		if (sv[i].notify_fd)
			snprintf(rdy, sizeof(rdy), "%u", sv[i].notify_fd);
		else
			snprintf(rdy, sizeof(rdy), "%s", st->none);

		printf("%s%3u%s  %s%-*s%s  %s%-7s%s  %3u  %5u  %s%-6s%s  %s%3s%s  %s%3s%s  ",
		       st->dim, i, st->reset,
		       col, (int)wname, ng_name(map, i), cend,
		       st->dim, ng_typename(sv[i].type), st->reset,
		       level[i], sv[i].n_desc,
		       pcol, ng_onfailname(pol), st->reset,
		       sv[i].notify_fd ? st->green : st->dim, rdy, st->reset,
		       ng_restart(map, i) ? st->green : st->dim,
		       ng_restart(map, i) ? "yes" : st->none, st->reset);

		if (doff[i] == doff[i + 1]) {
			printf("%s%s%s", st->dim, st->none, st->reset);
		} else {
			for (j = doff[i]; j < doff[i + 1]; j++)
				printf("%s%s", j > doff[i] ? " " : "", ng_name(map, didx[j]));
		}
		putchar('\n');

		if (verbose && sv[i].type != NG_TYPE_TARGET) {
			const char *p = ng_script(map, i);

			while (*p) {
				const char *nl = strchr(p, '\n');
				int w = nl ? (int)(nl - p) : (int)strlen(p);

				printf("%s     %s %.*s%s\n",
				       st->dim, st->vbar, w, p, st->reset);
				if (!nl)
					break;
				p = nl + 1;
			}
		}
	}

	{
		uint32_t *chain = calloc(maxlvl ? maxlvl : 1, sizeof(*chain));
		uint32_t cur = deepest, c = maxlvl;

		if (!chain) {
			fprintf(stderr, "ninitctl: out of memory\n");
			return 1;
		}
		while (c) {
			chain[--c] = cur;
			for (j = doff[cur]; j < doff[cur + 1]; j++)
				if (level[didx[j]] == level[cur] - 1) {
					cur = didx[j];
					break;
				}
		}

		printf("\n%s%scritical path%s %s(%u deep)%s\n  ",
		       st->bold, st->yellow, st->reset, st->dim, maxlvl, st->reset);
		for (i = 0; i < maxlvl; i++)
			printf("%s%s%s%s", i ? " " : "", i ? st->arrow : "",
			       i ? " " : "", ng_name(map, chain[i]));
		putchar('\n');
		free(chain);
	}

	return 0;
}
