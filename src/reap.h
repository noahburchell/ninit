#pragma once

#include <stdint.h>

void service_failed(uint32_t i, int status);
void start_failed(uint32_t i, int status);
void fire_restarts(void);
void reap(void);
