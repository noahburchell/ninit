#pragma once

#include <stdint.h>
#include <sys/types.h>

#define TAIL_CAP	1024
#define LINE_CAP	256
#define KILL_GRACE_MS	2000
#define DRAIN_POLL_CHUNKS 16
#define DRAIN_BUDGET	262144u
#define DRAIN_FINAL_CHUNKS 64
#define SHUTDOWN_DRAIN_MS 100

#define EXIT_NOEXEC	(127 << 8)

#define SVC_OP_NONE	0
#define SVC_OP_TERM	1
#define SVC_OP_KILL	2
#define SVC_OP_START	3

struct run {
	pid_t pid;
	pid_t pgid;
	pid_t stale_pid;
	int out_fd;
	int ntf_fd;
	uint32_t live_pos;
	long long restart_at;
	uint8_t burst;
	uint8_t cg_killed;
	uint8_t stale_hold;

	uint8_t attempt;
	uint8_t hup;
	uint8_t timedout;
	uint8_t starting;
	uint8_t ntf_exec;
	uint8_t op;
	uint8_t op_restart;
	uint16_t tail_len;
	uint16_t line_len;
	long long started;
	long long op_at;
	char tail[TAIL_CAP];
	char line[LINE_CAP];
};

extern const void *map;
extern uint32_t n_svc;
extern uint8_t *state;
extern uint8_t *up;
extern uint8_t *want;
extern uint16_t *unmet;
extern struct run *runs;
extern uint32_t *live, n_live;
extern uint32_t *queue;
extern uint32_t *rqueue;
extern uint8_t *released;
extern uint32_t n_active, n_done, n_pending, n_up;
extern uint32_t drain_left;
extern uint32_t drain_rotor;
extern int shutting_down;
extern int null_fd;

long long now_ms(void);
