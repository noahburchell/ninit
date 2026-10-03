#include "pidmap.h"
#include "ngraph.h"
#include "ninit.h"

#include <stdint.h>
#include <sys/types.h>

pid_t *pid_key;
uint32_t *pid_val, pid_mask;

static uint32_t pid_hash(pid_t p)
{
	return (uint32_t)((uint32_t)p * 0x9e3779b1u) & pid_mask;
}

void pid_put(pid_t pid, uint32_t svc)
{
	uint32_t i;

	if (!pid_key)
		return;
	for (i = pid_hash(pid); pid_key[i]; i = (i + 1) & pid_mask)
		if (pid_key[i] == pid)
			break;
	pid_key[i] = pid;
	pid_val[i] = svc;
}

void pid_del(pid_t pid)
{
	uint32_t i, j, k;

	if (!pid_key)
		return;
	for (i = pid_hash(pid); pid_key[i]; i = (i + 1) & pid_mask)
		if (pid_key[i] == pid)
			break;
	if (!pid_key[i])
		return;

	for (j = i;;) {
		pid_key[i] = 0;
		for (;;) {
			j = (j + 1) & pid_mask;
			if (!pid_key[j])
				return;
			k = pid_hash(pid_key[j]);
			if (i <= j ? (k <= i || k > j) : (k <= i && k > j))
				break;
		}
		pid_key[i] = pid_key[j];
		pid_val[i] = pid_val[j];
		i = j;
	}
}

void live_add(uint32_t i)
{
	runs[i].live_pos = n_live;
	live[n_live++] = i;
}

int live_has(uint32_t i)
{
	return runs[i].live_pos < n_live && live[runs[i].live_pos] == i;
}

void live_del(uint32_t i)
{
	uint32_t pos = runs[i].live_pos, last = live[--n_live];

	live[pos] = last;
	runs[last].live_pos = pos;
}

uint32_t find_pid(pid_t pid, int *stale)
{
	uint32_t i, k;

	if (pid_key) {
		for (i = pid_hash(pid); pid_key[i]; i = (i + 1) & pid_mask)
			if (pid_key[i] == pid) {
				*stale = runs[pid_val[i]].pid != pid;
				return pid_val[i];
			}
		return UINT32_MAX;
	}

	for (k = 0; k < n_live; k++) {
		uint32_t j = live[k];

		if (runs[j].pid == pid) {
			*stale = 0;
			return j;
		}
		if (runs[j].stale_pid == pid) {
			*stale = 1;
			return j;
		}
	}
	return UINT32_MAX;
}
