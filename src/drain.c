#include "drain.h"
#include "cgroup.h"
#include "fail.h"
#include "logging.h"
#include "ngraph.h"
#include "ninit.h"
#include "pidmap.h"
#include "reap.h"
#include "state.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void close_fds(uint32_t i)
{
	struct run *r = &runs[i];

	if (r->out_fd >= 0) {
		close(r->out_fd);
		r->out_fd = -1;
	}
	if (r->ntf_fd >= 0) {
		close(r->ntf_fd);
		r->ntf_fd = -1;
	}
}

void maybe_free(uint32_t i)
{
	struct run *r = &runs[i];

	if (r->pid == 0 && !r->stale_pid && r->out_fd < 0 && r->ntf_fd < 0 &&
	    !r->restart_at && r->op == SVC_OP_NONE && live_has(i)) {
		cgroup_drop(i);
		live_del(i);
	}
}

static void flush_line(uint32_t i)
{
	struct run *r = &runs[i];

	if (r->line_len)
		log_note("%s: %.*s", ng_name(map, i), (int)r->line_len, r->line);
	r->line_len = 0;
}

static void absorb(uint32_t i, const char *buf, size_t len)
{
	struct run *r = &runs[i];

	if (len >= TAIL_CAP) {
		memcpy(r->tail, buf + len - TAIL_CAP, TAIL_CAP);
		r->tail_len = TAIL_CAP;
	} else {
		if (r->tail_len + len > TAIL_CAP) {
			size_t keep = TAIL_CAP - len;

			memmove(r->tail, r->tail + r->tail_len - keep, keep);
			r->tail_len = (uint16_t)keep;
		}
		memcpy(r->tail + r->tail_len, buf, len);
		r->tail_len += (uint16_t)len;
	}

	for (size_t k = 0; k < len; k++) {
		if (buf[k] == '\n') {
			flush_line(i);
			continue;
		}
		if (r->line_len == LINE_CAP)
			flush_line(i);
		r->line[r->line_len++] = buf[k];
	}
}

void drain_out(uint32_t i, unsigned chunks, int budgeted)
{
	struct run *r = &runs[i];
	char buf[4096];

	while (r->out_fd >= 0 && chunks--) {
		size_t take = sizeof(buf);
		ssize_t k;

		if (budgeted) {
			if (!drain_left) {
				drain_rotor = i;
				return;
			}
			if (drain_left < take)
				take = drain_left;
		}
		k = read(r->out_fd, buf, take);

		if (k > 0) {
			if (budgeted)
				drain_left -= (uint32_t)k;
			absorb(i, buf, (size_t)k);
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && errno == EAGAIN)
			return;
		close(r->out_fd);
		r->out_fd = -1;
		flush_line(i);
	}
}

void drain_notify(uint32_t i, unsigned chunks)
{
	struct run *r = &runs[i];
	char buf[256];

	while (r->ntf_fd >= 0 && chunks--) {
		ssize_t k = read(r->ntf_fd, buf, sizeof(buf));

		if (k > 0) {
			if (r->starting && !r->timedout && !r->hup && memchr(buf, '\n', (size_t)k)) {
				long long ms = now_ms() - r->started;

				r->starting = 0;
				log_done("%s (%lld ms)", ng_name(map, i), ms);
				if (state[i] == NG_ST_RUNNING || state[i] == NG_ST_DONE)
					complete(i);
			}
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && errno == EAGAIN)
			return;

		close(r->ntf_fd);
		r->ntf_fd = -1;
		if (!r->starting) {
			maybe_free(i);
			return;
		}

		if (r->pid > 0) {
			r->hup = 1;
			signal_group(i, SIGKILL);
			return;
		}
		start_failed(i, FAIL_ST_NOTIFY_HUP);
		return;
	}
}

void drain_exec(uint32_t i)
{
	struct run *r = &runs[i];
	char buf[8];

	while (r->ntf_fd >= 0) {
		ssize_t k = read(r->ntf_fd, buf, sizeof(buf));

		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && errno == EAGAIN)
			return;

		close(r->ntf_fd);
		r->ntf_fd = -1;
		if (k > 0 || !r->starting)
			return;

		long long ms = now_ms() - r->started;

		r->starting = 0;
		log_done("%s (%lld ms)", ng_name(map, i), ms);
		if (state[i] == NG_ST_RUNNING || state[i] == NG_ST_DONE)
			complete(i);
	}
}

void drain_output(void)
{
	uint32_t k = n_live;

	while (k--) {
		uint32_t i = live[k];

		if (runs[i].out_fd >= 0)
			drain_out(i, DRAIN_POLL_CHUNKS, 1);
	}
}

void drain_all(void)
{
	uint32_t k;

	for (k = 0; k < n_live; ) {
		uint32_t i = live[k];

		if (runs[i].out_fd >= 0)
			drain_out(i, DRAIN_POLL_CHUNKS, 1);
		if (runs[i].ntf_fd >= 0) {
			if (runs[i].ntf_exec)
				drain_exec(i);
			else
				drain_notify(i, DRAIN_POLL_CHUNKS);
		}
		if (live_has(i))
			maybe_free(i);
		// dropping a service swaps another one into this slot
		if (k < n_live && live[k] == i)
			k++;
	}
}
