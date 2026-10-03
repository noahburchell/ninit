#include "reap.h"
#include "cgroup.h"
#include "drain.h"
#include "fail.h"
#include "logging.h"
#include "ngraph.h"
#include "ninit.h"
#include "pidmap.h"
#include "spawn.h"
#include "state.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>

#define RETRY_MIN_MS	10
#define RESTART_SETTLED_MS 10000

static const unsigned restart_delay_ms[] = { 100, 250, 500, 1000, 2000, 5000 };

static unsigned retry_delay_ms(uint32_t i)
{
	unsigned d = ng_pol(map, i)->retry_ms;

	return d < RETRY_MIN_MS ? RETRY_MIN_MS : d;
}

void service_failed(uint32_t i, int status)
{
	struct run *r = &runs[i];
	enum fail_act act;
	char why[160];

	kill_group(i);
	drain_out(i, DRAIN_FINAL_CHUNKS, 0);
	close_fds(i);
	r->starting = 0;

	svc_abandon(i);
	if (live_has(i) && !r->stale_pid && r->op == SVC_OP_NONE)
		live_del(i);

	if (shutting_down)
		return;

	if (r->attempt < UINT8_MAX)
		r->attempt++;
	act = fail_service(map, i, status, r->attempt, ng_start_tries(map, i),
			   r->tail, r->tail_len);

	switch (act) {
	case FAIL_RETRY:
		if (!live_has(i))
			live_add(i);
		r->stale_hold = r->stale_pid != 0;
		r->restart_at = now_ms() + (r->stale_pid ? KILL_GRACE_MS : retry_delay_ms(i));
		return;
	case FAIL_SHELL:
		poison(i);
		snprintf(why, sizeof(why), "boot: cannot continue without %s", ng_name(map, i));
		fail_emergency_shell(why);
		return;
	case FAIL_WARN:
	case FAIL_STOP:
	default:
		poison(i);
		return;
	}
}

static void give_up(uint32_t i, int status)
{
	struct run *r = &runs[i];
	unsigned tries = ng_start_tries(map, i);
	char why[160];

	if (fail_service(map, i, status, tries, tries, r->tail, r->tail_len) == FAIL_SHELL) {
		poison(i);
		snprintf(why, sizeof(why), "boot: cannot continue without %s", ng_name(map, i));
		fail_emergency_shell(why);
		return;
	}
	poison(i);
}

static void restart_schedule(uint32_t i, int status)
{
	struct run *r = &runs[i];
	unsigned last = sizeof(restart_delay_ms) / sizeof(*restart_delay_ms) - 1;
	long long alive = now_ms() - r->started;
	unsigned d;
	int first;
	char how[64];

	if (alive >= RESTART_SETTLED_MS)
		r->burst = 0;
	first = r->burst == 0;
	d = restart_delay_ms[r->burst < last ? r->burst : last];
	if (r->burst < last)
		r->burst++;

	kill_group(i);
	drain_out(i, DRAIN_FINAL_CHUNKS, 0);
	close_fds(i);
	svc_abandon(i);
	r->restart_at = now_ms() + d;
	r->stale_hold = 0;

	fail_describe(status, how, sizeof(how));
	log_warn("%s: exited (%s) after %lld ms up, restarting in %u ms",
		 ng_name(map, i), how, alive, d);

	if (first && r->tail_len)
		log_raw(LOG_WARN, r->tail, r->tail_len);
}

void start_failed(uint32_t i, int status)
{
	runs[i].starting = 0;
	if (state[i] == NG_ST_RUNNING) {
		service_failed(i, status);
		return;
	}
	if (!shutting_down)
		go_down(i);
	if (!shutting_down && ng_restart(map, i)) {
		restart_schedule(i, status);
		return;
	}
	close_fds(i);
	if (!shutting_down)
		poison_deps(i);
	maybe_free(i);
}

static void fire_restart(uint32_t i, long long now)
{
	struct run *r = &runs[i];
	const char *why;

	if (!r->restart_at || now < r->restart_at)
		return;
	r->restart_at = 0;
	if (want[i])
		return;

	if (!svc_can_start(i, &why)) {
		if (r->stale_pid && state[i] == NG_ST_RUNNING) {
			log_err("%s: pid %d has not exited, not restarting", ng_name(map, i),
				(int)r->stale_pid);
			give_up(i, FAIL_ST_TIMEOUT);
			return;
		}
		if (r->stale_pid || r->pid > 0 || state[i] == NG_ST_RUNNING) {
			r->restart_at = now + KILL_GRACE_MS;
			r->stale_hold = r->stale_pid != 0;
		}
		return;
	}

	// launch() sets it too but a failure here still has to date the backoff from now
	r->started = now;
	if (launch(i) < 0) {
		if (state[i] == NG_ST_RUNNING)
			service_failed(i, EXIT_NOEXEC);
		else
			restart_schedule(i, EXIT_NOEXEC);
	}
}

void fire_restarts(void)
{
	long long now = now_ms();
	uint32_t k;

	if (shutting_down)
		return;

	for (k = 0; k < n_live; ) {
		uint32_t i = live[k];

		fire_restart(i, now);
		// dropping a service swaps another one into this slot
		if (k < n_live && live[k] == i)
			k++;
	}
}

long long restarts_due(void)
{
	long long best = -1, now = now_ms();
	uint32_t k;

	for (k = 0; k < n_live; k++) {
		struct run *r = &runs[live[k]];
		long long d;

		if (!r->restart_at)
			continue;
		d = r->restart_at - now;
		if (d < 0)
			d = 0;
		if (best < 0 || d < best)
			best = d;
	}

	return best;
}

long long starts_due(void)
{
	long long best = -1, now = now_ms();
	uint32_t k;

	for (k = 0; k < n_live; k++) {
		struct run *r = &runs[live[k]];
		long long d;

		if (!r->starting || r->timedout)
			continue;
		d = r->started + ng_start_ms(map, live[k]) - now;
		if (d < 0)
			d = 0;
		if (best < 0 || d < best)
			best = d;
	}

	return best;
}

// only the sigkill drain_notify() sent says less than the hup itself
static int hup_status(int status)
{
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL ? FAIL_ST_NOTIFY_HUP : status;
}

static void child_exited(uint32_t i, int status)
{
	struct run *r = &runs[i];
	const char *name = ng_name(map, i);
	char how[64];

	drain_out(i, DRAIN_FINAL_CHUNKS, 0);

	if (r->ntf_fd >= 0 && !r->ntf_exec)
		drain_notify(i, DRAIN_FINAL_CHUNKS);
	pid_del(r->pid);
	r->pid = 0;

	if (want[i] && !shutting_down) {
		r->starting = 0;
		r->restart_at = 0;
		go_down(i);
		// the stop operation sends SIGKILL to what is left once stop-timeout passes
		if (!cg_ok || r->op == SVC_OP_NONE)
			kill_group(i);
		drain_out(i, DRAIN_FINAL_CHUNKS, 0);
		close_fds(i);
		if (state[i] == NG_ST_RUNNING) {
			n_active--;
			state[i] = NG_ST_PENDING;
			n_pending++;
		}
		maybe_free(i);
		return;
	}
	if (state[i] != NG_ST_RUNNING) {
		r->starting = 0;
		if (state[i] == NG_ST_DONE && !shutting_down) {
			if (r->timedout)
				status = FAIL_ST_TIMEOUT;
			else if (r->hup)
				status = hup_status(status);
			go_down(i);
			if (ng_restart(map, i)) {
				restart_schedule(i, status);
				return;
			}
			fail_describe(status, how, sizeof(how));
			log_warn("%s: exited (%s) after %lld ms up", name, how,
				 now_ms() - r->started);
			kill_group(i);
			drain_out(i, DRAIN_FINAL_CHUNKS, 0);
			if (r->tail_len)
				log_raw(LOG_WARN, r->tail, r->tail_len);
			close_fds(i);
			poison_deps(i);
		}
		maybe_free(i);
		return;
	}
	if (shutting_down) {
		close_fds(i);
		maybe_free(i);
		return;
	}
	if (r->timedout) {
		service_failed(i, FAIL_ST_TIMEOUT);
		return;
	}
	if (r->hup) {
		service_failed(i, hup_status(status));
		return;
	}

	if (ng_svcs(map)[i].type == NG_TYPE_ONESHOT) {
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			long long ms = now_ms() - r->started;

			log_done("%s (%lld ms)", name, ms);
			complete(i);
			maybe_free(i);
		} else {
			service_failed(i, status);
		}
		return;
	}

	service_failed(i, status);
}

// an abandoned instance finally died
static void stale_reaped(uint32_t i, pid_t pid)
{
	struct run *r = &runs[i];

	pid_del(pid);
	if (r->stale_pid == pid)
		r->stale_pid = 0;
	// a start that only waited for this pid goes ahead, a restart backoff runs its course
	if (r->restart_at && r->stale_hold) {
		r->restart_at = now_ms() + retry_delay_ms(i);
		r->stale_hold = 0;
	}
	maybe_free(i);
}

void reap(void)
{
	for (;;) {
		int status, stale;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		uint32_t i;

		if (pid == 0)
			return;
		if (pid < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		// nothing is restarted once shutdown has begun
		if (!shutting_down && fail_emergency_reaped(pid, status))
			continue;
		i = find_pid(pid, &stale);
		if (i == UINT32_MAX)
			continue;
		if (stale)
			stale_reaped(i, pid);
		else
			child_exited(i, status);
	}
}
