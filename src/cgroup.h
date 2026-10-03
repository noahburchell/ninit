#pragma once

#include "ngraph.h"

#include <stddef.h>
#include <stdint.h>

#define CG_BASE		"/sys/fs/cgroup"
#define CG_DIR		CG_BASE "/ninit.services"
#define CG_PATH_MAX	(sizeof(CG_DIR) + NG_MAX_NAME + 24)

extern int cg_ok;

void cgroup_init(void);
int cg_path(uint32_t i, const char *leaf, char *buf, size_t cap);
void cgroup_make(uint32_t i, char *procs, size_t cap);
int cgroup_populated(uint32_t i);
int cgroup_drop(uint32_t i);
int cgroup_signal(uint32_t i, int sig);
