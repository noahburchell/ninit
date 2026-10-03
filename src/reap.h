#pragma once

#include <stdint.h>

void service_failed(uint32_t i, int status);
void start_failed(uint32_t i, int status);
void fire_restarts(void);
long long restarts_due(void);
long long starts_due(void);
void reap(void);
