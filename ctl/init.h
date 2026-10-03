#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct src;

struct edge {
	uint32_t a, b;
};

struct graph {
	uint32_t n;
	uint32_t *off;
	uint32_t *idx;
	uint32_t m;
	uint32_t *indeg;
	uint32_t *height;
	uint8_t *color;
	uint32_t *path;
	uint32_t *iter;
	uint32_t depth;
};

struct build {
	const char *dir;
	struct src *srcs;
	uint32_t n;
	struct edge *edges;
	uint32_t m;
	struct graph g;
	uint32_t *order;
	uint32_t nroots;
	uint64_t hash;
	mode_t out_mode;
};

void read_sources(struct build *b, const char *out_base, size_t out_base_len);
void check_syntax(struct src *srcs, uint32_t n, const char *dir);
void link_graph(struct build *b);
void order_graph(struct build *b);
void write_image(const struct build *b, const char *out, int dry, int srclock);
const char *output_base(const char *dir, const char *out, size_t *out_base_len);
int graph_image(const char *path);
void refuse_nongraph(const char *path);
int lock_dir(const char *path);
void write_atomic(const char *path, const void *buf, size_t len, mode_t mode, int held);
