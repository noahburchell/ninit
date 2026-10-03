#pragma once

#include <stdint.h>

void close_fds(uint32_t i);
void maybe_free(uint32_t i);
void drain_out(uint32_t i, unsigned chunks, int budgeted);
void drain_notify(uint32_t i, unsigned chunks);
void drain_exec(uint32_t i);
void drain_output(void);
void drain_all(void);
