#pragma once

#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#define NG_MAGIC	0x4744494eu // 'NIDG'
#define NG_VERSION	3u

#define NG_TYPE_ONESHOT	0
#define NG_TYPE_DAEMON	1
#define NG_TYPE_TARGET	2

#define NG_ONFAIL_MASK	0x03
#define NG_ONFAIL_WARN	0
#define NG_ONFAIL_STOP	1
#define NG_ONFAIL_SHELL	2

// a target is a sync point with no script
#define NG_NO_SCRIPT	UINT32_MAX

#define NG_SHELL	"/bin/bash"

// kernel doesnt give path, bash does this for us but maybe better to hardcode
#define NG_PATH	"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

static_assert(__builtin_strncmp(NG_PATH, "PATH=", 5) == 0, "NG_PATH must be a putenv-style entry");

#define NG_MAX_SCRIPT	131071u

#define NG_DEFAULT_DIR	"/etc/ninit/ninit.d"
#define NG_DEFAULT_FILE	"/etc/ninit/depgraph"

struct ng_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t crc32;
	uint32_t total_len;
	uint32_t n_svc;
	uint32_t n_roots;
	uint32_t n_edges;
	uint32_t blob_len;
	uint32_t off_svc;
	uint32_t off_rdep_off;
	uint32_t off_rdep_idx;
	uint32_t off_blob;
	uint32_t reserved[2];
	uint64_t srcs_hash;
};

struct ng_svc {
	uint16_t unmet;
	uint8_t type;
	uint8_t flags;
	uint16_t n_desc;
	uint16_t pad;
	uint32_t script_off;
	uint32_t name_off;
};

static_assert(sizeof(struct ng_hdr) == 64, "header must be one cache line");
static_assert(sizeof(struct ng_svc) == 16, "four services per cache line");

static inline const struct ng_svc *ng_svcs(const void *m)
{
	return (const void *)((const char *)m + ((const struct ng_hdr *)m)->off_svc);
}

static inline const uint32_t *ng_rdep_off(const void *m)
{
	return (const void *)((const char *)m + ((const struct ng_hdr *)m)->off_rdep_off);
}

static inline const uint32_t *ng_rdep_idx(const void *m)
{
	return (const void *)((const char *)m + ((const struct ng_hdr *)m)->off_rdep_idx);
}

static inline const char *ng_blob(const void *m)
{
	return (const char *)m + ((const struct ng_hdr *)m)->off_blob;
}

static inline const char *ng_name(const void *m, uint32_t i)
{
	return ng_blob(m) + ng_svcs(m)[i].name_off;
}

static inline const char *ng_script(const void *m, uint32_t i)
{
	return ng_blob(m) + ng_svcs(m)[i].script_off;
}

static inline uint8_t ng_onfail(const void *m, uint32_t i)
{
	return ng_svcs(m)[i].flags & NG_ONFAIL_MASK;
}

uint32_t ng_crc32c(const void *data, size_t len);

const char *ng_verify(const void *map, size_t len);

const char *ng_typename(uint8_t type);
const char *ng_onfailname(uint8_t policy);
