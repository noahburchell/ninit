#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define NG_ST_PENDING	0
#define NG_ST_RUNNING	1
#define NG_ST_DONE	2
#define NG_ST_FAILED	3
#define NG_ST_SKIPPED	4

enum fail_act {
	FAIL_RETRY,
	FAIL_WARN,
	FAIL_STOP,
	FAIL_SHELL,
};

#define FAIL_ST_NOTIFY_HUP	(-1)
#define FAIL_ST_TIMEOUT		(-2)

void fail_describe(int status, char *buf, size_t cap);

enum fail_act fail_service(const void *map, uint32_t i, int status, unsigned attempt,
			   const char *tail, size_t tail_len);

uint32_t fail_poison(const void *map, uint32_t i, uint8_t *state);

void ninit_cloexec_except(int keep);

void ninit_oom_score_adj(const char *v, size_t len);

void fail_emergency_shell(const char *why);

int fail_emergency_reaped(pid_t pid, int status);

void fail_summary(const void *map, const uint8_t *state);
