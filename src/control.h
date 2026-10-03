#pragma once

#include "nctl.h"

#include <stdint.h>

#define CTL_MAX		8
#define CTL_BUF		NCTL_REQ_MAX
#define CTL_OUT		8192

struct ctl {
	int fd;
	uint32_t svc;
	uint32_t list_at;
	uint64_t log_at;
	uint8_t listing;
	uint8_t done;
	uint8_t watch;
	uint16_t in_len;
	uint16_t out_at, out_len;
	char in[CTL_BUF];
	char out[CTL_OUT];
};

extern int ctl_lfd;
extern uint32_t n_ops;
extern struct ctl ctl_conn[CTL_MAX];
extern int n_ctl;

const char *state_name(uint32_t i);
void ctl_init(void);
void ctl_out(struct ctl *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ctl_end(struct ctl *c, int ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void ctl_feed(void);
void ctl_pump(struct ctl *c);
void ctl_read(struct ctl *c);
void ctl_accept(void);
long long ctl_due(void);
