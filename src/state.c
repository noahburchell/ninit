#include "state.h"
#include "cgroup.h"
#include "fail.h"
#include "logging.h"
#include "ngraph.h"
#include "ninit.h"
#include "pidmap.h"
#include "spawn.h"

#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>

static uint32_t q_head, q_tail;
static int draining;

void signal_group(uint32_t i, int sig)
{
	cgroup_signal(i, sig);
	if (runs[i].pgid > 0)
		kill(-runs[i].pgid, sig);
}

void kill_group(uint32_t i)
{
	signal_group(i, SIGKILL);
	runs[i].pgid = 0;
}

void svc_abandon(uint32_t i)
{
	struct run *r = &runs[i];

	if (r->pid <= 0)
		return;
	r->stale_pid = r->pid;
	r->pid = 0;
}

int svc_gone(uint32_t i)
{
	const struct run *r = &runs[i];

	if (r->pid > 0 || r->stale_pid)
		return 0;
	return !r->started || cgroup_populated(i) <= 0;
}

int svc_can_start(uint32_t i, const char **why)
{
	const struct run *r = &runs[i];

	if (shutting_down) {
		*why = "shutting down";
		return 0;
	}
	if (want[i]) {
		*why = "held stopped";
		return 0;
	}
	if (r->pid > 0) {
		*why = "already running";
		return 0;
	}
	if (r->stale_pid) {
		*why = "its previous instance has not exited";
		return 0;
	}
	if (unmet[i]) {
		*why = "waiting on a prerequisite";
		return 0;
	}
	return 1;
}

int svc_try_start(uint32_t i)
{
	const char *why;

	if (!svc_can_start(i, &why))
		return 0;
	spawn(i);
	return 1;
}

static void svc_mark_done(uint32_t i)
{
	runs[i].starting = 0;
	if (state[i] == NG_ST_RUNNING)
		n_active--;
	else if (state[i] == NG_ST_PENDING)
		n_pending--;
	if (state[i] != NG_ST_DONE) {
		state[i] = NG_ST_DONE;
		n_done++;
	}
}

void svc_mark_pending(uint32_t i)
{
	if (state[i] == NG_ST_PENDING)
		return;
	if (state[i] == NG_ST_RUNNING)
		n_active--;
	else if (state[i] == NG_ST_DONE)
		n_done--;
	state[i] = NG_ST_PENDING;
	n_pending++;
}

static void svc_set_up(uint32_t i, int v)
{
	if (!up[i] == !v)
		return;
	up[i] = (uint8_t)!!v;
	if (v)
		n_up++;
	else
		n_up--;
}

static void svc_release(uint32_t i)
{
	const uint32_t *roff = ng_rdep_off(map), *ridx = ng_rdep_idx(map);

	if (released[i])
		return;
	released[i] = 1;
	queue[q_tail++] = i;
	if (draining)
		return;

	draining = 1;
	while (q_head < q_tail) {
		uint32_t j = queue[q_head++], k;

		for (k = roff[j]; k < roff[j + 1]; k++) {
			uint32_t d = ridx[k];

			if (--unmet[d] != 0 || want[d])
				continue;
			if (ng_svcs(map)[d].type == NG_TYPE_TARGET) {
				if (released[d])
					continue;
				svc_set_up(d, 1);
				svc_mark_done(d);
				log_done("%s", ng_name(map, d));
				released[d] = 1;
				queue[q_tail++] = d;
				continue;
			}
			if (state[d] == NG_ST_PENDING) {
				svc_try_start(d);
			} else if (state[d] == NG_ST_DONE && !up[d] && ng_restart(map, d)) {
				if (!live_has(d))
					live_add(d);
				if (!runs[d].restart_at)
					runs[d].restart_at = now_ms();
			}
		}
	}
	draining = 0;
	q_head = q_tail = 0;
}

static void svc_reclaim(uint32_t i)
{
	const uint32_t *roff = ng_rdep_off(map), *ridx = ng_rdep_idx(map);
	uint32_t rh = 0, rt = 0;

	if (!released[i] || ng_order_only(map, i))
		return;
	released[i] = 0;
	rqueue[rt++] = i;

	while (rh < rt) {
		uint32_t j = rqueue[rh++], k;

		for (k = roff[j]; k < roff[j + 1]; k++) {
			uint32_t d = ridx[k];

			unmet[d]++;
			if (ng_svcs(map)[d].type != NG_TYPE_TARGET)
				continue;
			if (!released[d] || ng_order_only(map, d))
				continue;
			released[d] = 0;
			svc_set_up(d, 0);
			rqueue[rt++] = d;
		}
	}
}

void go_down(uint32_t i)
{
	if (!up[i] && !released[i])
		return;
	svc_set_up(i, 0);
	svc_reclaim(i);
}

void complete(uint32_t i)
{
	svc_mark_done(i);
	svc_set_up(i, 1);
	svc_release(i);
}

void poison(uint32_t i)
{
	uint32_t skipped, undone;

	if (state[i] == NG_ST_RUNNING)
		n_active--;
	else if (state[i] == NG_ST_PENDING)
		n_pending--;
	go_down(i);
	skipped = fail_poison(map, i, state, up, &undone);
	n_pending -= skipped;
	n_done -= undone;
	if (skipped)
		log_warn("%s: %u dependent service%s will not start", ng_name(map, i),
			 skipped, skipped == 1 ? "" : "s");
}

// a service that had completed is gone for good
void poison_deps(uint32_t i)
{
	uint32_t skipped, undone;

	if (ng_order_only(map, i))
		return;
	skipped = fail_poison_deps(map, i, state, up, &undone);

	n_pending -= skipped;
	n_done -= undone;
	if (skipped)
		log_warn("%s: %u dependent service%s will not start", ng_name(map, i),
			 skipped, skipped == 1 ? "" : "s");
}
