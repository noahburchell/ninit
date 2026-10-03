#include "command.h"
#include "control.h"
#include "drain.h"
#include "fail.h"
#include "logging.h"
#include "nctl.h"
#include "ngraph.h"
#include "ninit.h"
#include "pidmap.h"
#include "spawn.h"
#include "state.h"

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>

#define CTL_WATCH	4

static uint32_t ctl_find(const char *name)
{
	uint32_t i;

	for (i = 0; i < n_svc; i++)
		if (!strcmp(ng_name(map, i), name))
			return i;
	return UINT32_MAX;
}

// every connection watching this service learns how its operation ended
static void svc_op_set(uint32_t i, uint8_t op, long long at)
{
	if ((runs[i].op == SVC_OP_NONE) != (op == SVC_OP_NONE)) {
		if (op == SVC_OP_NONE)
			n_ops--;
		else
			n_ops++;
	}
	runs[i].op = op;
	runs[i].op_at = at;
}

static void svc_op_done(uint32_t i, int ok, const char *msg)
{
	int k;

	svc_op_set(i, SVC_OP_NONE, 0);
	runs[i].op_restart = 0;
	for (k = 0; k < n_ctl; k++)
		if (ctl_conn[k].svc == i)
			ctl_end(&ctl_conn[k], ok, "%s %s", ng_name(map, i), msg);
	maybe_free(i);
}

static void svc_op_start(uint32_t i)
{
	struct run *r = &runs[i];
	const char *why = "";

	want[i] = 0;
	if (!svc_can_start(i, &why)) {
		if (!shutting_down && r->pid <= 0 && !r->stale_pid) {
			r->attempt = 0;
			r->burst = 0;
			go_down(i);
			svc_mark_pending(i);
		}
		svc_op_done(i, 0, why);
		return;
	}
	r->attempt = 0;
	r->burst = 0;
	r->op_restart = 0;
	svc_mark_pending(i);
	if (!live_has(i))
		live_add(i);
	svc_op_set(i, SVC_OP_START, now_ms() + (long long)ng_start_ms(map, i) + KILL_GRACE_MS);
	spawn(i);
}

static void svc_op_stop(uint32_t i, int restart)
{
	struct run *r = &runs[i];

	want[i] = 1;
	r->restart_at = 0;
	r->op_restart = (uint8_t)restart;
	if (!live_has(i))
		live_add(i);

	if (svc_gone(i)) {
		if (restart) {
			svc_op_start(i);
			return;
		}
		// it exited on its own so nothing else will hand its dependents back
		go_down(i);
		svc_op_done(i, 1, "already stopped");
		return;
	}
	signal_group(i, SIGTERM);
	svc_op_set(i, SVC_OP_TERM, now_ms() + ng_stop_ms(map, i));
}

void ctl_tick(void)
{
	long long now;
	uint32_t k;

	if (!n_ops)
		return;
	now = now_ms();
	for (k = 0; k < n_live; ) {
		uint32_t i = live[k];
		struct run *r = &runs[i];
		int gone = svc_gone(i);

		switch (r->op) {
		case SVC_OP_TERM:
			if (gone) {
				if (r->op_restart)
					svc_op_start(i);
				else
					svc_op_done(i, 1, "stopped");
				break;
			}
			if (now < r->op_at)
				break;
			log_warn("%s: still running %u ms after SIGTERM, killing it",
				 ng_name(map, i), ng_stop_ms(map, i));
			signal_group(i, SIGKILL);
			svc_op_set(i, SVC_OP_KILL, now + KILL_GRACE_MS);
			break;

		case SVC_OP_KILL:
			if (gone) {
				if (r->op_restart)
					svc_op_start(i);
				else
					svc_op_done(i, 1, "stopped after SIGKILL");
				break;
			}
			if (now >= r->op_at)
				svc_op_done(i, 0, "did not stop after SIGKILL");
			break;

		case SVC_OP_START:
			if (state[i] == NG_ST_DONE && up[i]) {
				svc_op_done(i, 1, "up");
				break;
			}
			if (state[i] == NG_ST_FAILED || state[i] == NG_ST_SKIPPED) {
				svc_op_done(i, 0, state_name(i));
				break;
			}
			if (now >= r->op_at)
				svc_op_done(i, 0, "start timed out");
			break;

		default:
			break;
		}
		// finishing an operation swaps another service into this slot
		if (k < n_live && live[k] == i)
			k++;
	}
}

static void ctl_resume(struct ctl *c)
{
	uint32_t i, reset = 0, started = 0;

	for (i = 0; i < n_svc; i++) {
		if (state[i] != NG_ST_FAILED && state[i] != NG_ST_SKIPPED)
			continue;
		state[i] = NG_ST_PENDING;
		runs[i].attempt = 0;
		runs[i].burst = 0;
		n_pending++;
		reset++;
	}
	if (!reset) {
		ctl_end(c, 1, "nothing to resume");
		return;
	}
	for (i = 0; i < n_svc; i++) {
		if (state[i] != NG_ST_PENDING)
			continue;
		if (ng_svcs(map)[i].type == NG_TYPE_TARGET) {
			if (!unmet[i] && !want[i])
				complete(i);
			continue;
		}
		started += (uint32_t)svc_try_start(i);
	}
	log_note("control: resuming %u service%s, %u started", reset,
		 reset == 1 ? "" : "s", started);
	ctl_end(c, 1, "resuming %u service%s, %u started", reset,
		reset == 1 ? "" : "s", started);
}

void ctl_cmd(struct ctl *c, char *line)
{
	char *arg = strchr(line, ' ');
	uint32_t i;
	int restart;

	if (arg) {
		*arg++ = '\0';
		while (*arg == ' ')
			arg++;
		if (!*arg)
			arg = NULL;
	}

	if (!strcmp(line, "status")) {
		if (!arg) {
			c->listing = 1;
			c->list_at = 0;
			return;
		}
		i = ctl_find(arg);
		if (i == UINT32_MAX) {
			ctl_end(c, 0, "no service named %s", arg);
			return;
		}
		ctl_out(c, NCTL_DATA "%s %s %s pid %d\n", ng_name(map, i), state_name(i),
			want[i] ? "stopped" : "wanted", (int)runs[i].pid);
		ctl_end(c, 1, "%s", ng_name(map, i));
		return;
	}

	if (!strcmp(line, "log")) {
		int watching = 0;

		if (arg && strcmp(arg, "watch")) {
			ctl_end(c, 0, "unknown log mode '%s'", arg);
			return;
		}
		for (int k = 0; k < n_ctl; k++)
			watching += ctl_conn[k].watch;
		if (arg && watching >= CTL_WATCH) {
			ctl_end(c, 0, "too many log watchers");
			return;
		}
		c->watch = arg != NULL;
		c->listing = 2;
		c->log_at = log_first();
		return;
	}

	if (!strcmp(line, "reload")) {
		ctl_end(c, 0, "the running graph cannot be replaced, reboot to load a new one");
		return;
	}

	if (!strcmp(line, "resume")) {
		if (shutting_down)
			ctl_end(c, 0, "shutting down");
		else
			ctl_resume(c);
		return;
	}

	restart = !strcmp(line, "restart");
	if (!restart && strcmp(line, "stop") && strcmp(line, "start")) {
		ctl_end(c, 0, "unknown command '%s'", line);
		return;
	}
	if (!arg) {
		ctl_end(c, 0, "%s needs a service name", line);
		return;
	}
	if (shutting_down) {
		ctl_end(c, 0, "shutting down");
		return;
	}
	i = ctl_find(arg);
	if (i == UINT32_MAX) {
		ctl_end(c, 0, "no service named %s", arg);
		return;
	}
	if (ng_svcs(map)[i].type == NG_TYPE_TARGET) {
		ctl_end(c, 0, "%s is a target, it has no process", arg);
		return;
	}
	if (runs[i].op != SVC_OP_NONE) {
		ctl_end(c, 0, "%s is busy", arg);
		return;
	}

	log_note("control: %s %s", line, ng_name(map, i));
	c->svc = i;
	if (restart)
		svc_op_stop(i, 1);
	else if (!strcmp(line, "stop"))
		svc_op_stop(i, 0);
	else
		svc_op_start(i);
}
