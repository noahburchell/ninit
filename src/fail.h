#pragma once

#include <stdint.h>
#include <stddef.h>

#define NG_ST_PENDING	0
#define NG_ST_RUNNING	1
#define NG_ST_DONE	2
#define NG_ST_FAILED	3
#define NG_ST_SKIPPED	4

int fail_service(const void *map, uint32_t i, int status, unsigned attempt,
		 const char *tail, size_t tail_len);

uint32_t fail_poison(const void *map, uint32_t i, uint8_t *state);

void fail_emergency_shell(const char *why);

void fail_summary(const void *map, const uint8_t *state);
