#pragma once

#include "run.h"

#include <stdint.h>

extern const void* map;
extern uint32_t n_svc;
extern uint8_t* state;
extern uint8_t* up;
extern uint8_t* want;
extern uint16_t* unmet;
extern struct run* runs;
extern uint32_t* live;
extern uint32_t n_live;
extern uint32_t* queue;
extern uint32_t* rqueue;
extern uint8_t* released;
extern uint32_t n_active, n_done, n_pending, n_up;
extern uint32_t drain_left;
extern uint32_t drain_rotor;
extern bool shutting_down;
extern int null_fd;
extern int64_t boot_ms;

int64_t now_ms(void);
