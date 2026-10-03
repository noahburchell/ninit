#pragma once

#include "ngraph.h"

#include <sys/resource.h>

extern char env_locale[NG_LOCALE_MAX][NG_LOCALE_LEN];
extern int n_locale;
extern struct rlimit child_nofile;

void ensure_stdio(void);
void mount_api_fs(void);
void seed_dev(void);
void raise_nofile(void);
const void *load_graph(const char *path, const char **why);
void load_locale(void);
