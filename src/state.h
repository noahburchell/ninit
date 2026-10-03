#pragma once

#include <stdint.h>

void signal_group(uint32_t i, int sig);
void kill_group(uint32_t i);
void svc_abandon(uint32_t i);
int svc_gone(uint32_t i);
int svc_can_start(uint32_t i, const char **why);
int svc_try_start(uint32_t i);
void svc_mark_pending(uint32_t i);
void go_down(uint32_t i);
void complete(uint32_t i);
void poison(uint32_t i);
void poison_deps(uint32_t i);
