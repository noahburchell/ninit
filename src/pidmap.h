#pragma once

#include <stdint.h>
#include <sys/types.h>

extern pid_t *pid_key;
extern uint32_t *pid_val, pid_mask;

void pid_put(pid_t pid, uint32_t svc);
void pid_del(pid_t pid);
void live_add(uint32_t i);
int live_has(uint32_t i);
void live_del(uint32_t i);
uint32_t find_pid(pid_t pid, int *stale);
