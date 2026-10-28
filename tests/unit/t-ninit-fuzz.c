#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "sim.h"

static void random_graph(struct gb_svc* sv, uint32_t* n, struct gb_edge* e, uint32_t* m) {
	static char names[16][8];
	uint32_t roots, k;

	*n = 3 + tap_below(10);
	*m = 0;
	roots = 1 + tap_below(*n / 2 + 1);
	for(k = 0; k < *n; k++) {
		snprintf(names[k], sizeof(names[k]), "s%u", k);
		sv[k] = (struct gb_svc){ .name = names[k], .onfail = GB_DEFAULT, .script = "x" };
		switch(tap_below(4)) {
			case 0:
				sv[k].type = NG_TYPE_TARGET;
				break;
			case 1:
			case 2:
				sv[k].type = NG_TYPE_DAEMON;
				sv[k].restart = (uint8_t)tap_below(2);
				sv[k].notify = tap_below(2) ? 3 : 0;
				break;
			default:
				sv[k].type = NG_TYPE_ONESHOT;
		}

		sv[k].pol.start_ms = 100 + tap_below(3) * 500;
		sv[k].pol.stop_ms = 100 + tap_below(2) * 300;
		sv[k].pol.start_tries = (uint8_t)(1 + tap_below(3));
		sv[k].pol.retry_ms = (uint16_t)tap_below(200);
		sv[k].pol.pflags = tap_below(4) ? 0 : NG_PF_ORDER_ONLY;
		if(k < roots) {
			continue;
		}

		// a non-root needs a prerequisite before it
		e[(*m)++] = (struct gb_edge){ tap_below(k), k };
		if(k > 1 && tap_below(2)) {
			uint32_t a = tap_below(k);

			if(a != e[*m - 1].a) {
				e[(*m)++] = (struct gb_edge){ a, k };
			}
		}
	}

	for(k = 0; k < *n; k++) {
		int has_dep = 0;

		for(uint32_t j = 0; j < *m; j++) {
			has_dep |= e[j].a == k;
		}

		if(!has_dep && !tap_below(4)) {
			sv[k].onfail = NG_ONFAIL_WARN;
		} else if(tap_below(6) == 0) {
			sv[k].onfail = NG_ONFAIL_SHELL;
		}
	}
}

static int sim_trace;

static void trace_states(void) {
	if(!sim_trace) {
		return;
	}

	fputs("#     ", stdout);
	for(uint32_t i = 0; i < n_svc; i++) {
		printf(" %s=%s%s%s", ng_name(map, i), state_name(i), up[i] ? "^" : "", want[i] ? "!" : "");
	}

	putchar('\n');
}

static void test_sim_fuzz(void) {
	static const char* const verbs[] = { "start", "stop", "restart", "status", "resume" };
	struct gb_svc sv[16];
	struct gb_edge e[32];
	uint32_t n, m;
	int bad = 0, graphs = 3000, steps = 400;

	const char* only = getenv("NINIT_SIM_SEED");

	if(only) {
		tap_rng_state = strtoull(only, NULL, 0);
		graphs = 1;
		sim_trace = 1;
	}

	for(int g = 0; g < graphs && !bad; g++) {
		uint64_t seed = tap_rng_state;

		random_graph(sv, &n, e, &m);
		sim_load(sv, n, e, m);
		if(sim_trace) {
			for(uint32_t k = 0; k < n; k++) {
				printf("# svc %s type %s restart %d notify %d onfail %d tries %d order %d\n",
				       sv[k].name, ng_typename(sv[k].type), sv[k].restart, sv[k].notify,
				       ng_onfail(map, k), sv[k].pol.start_tries, sv[k].pol.pflags);
			}
		}

		for(int s = 0; s < steps && !bad; s++) {
			uint32_t i = tap_below(n);
			uint32_t act = tap_below(8);
			const char* why;

			if(sim_trace) {
				printf("# step %d action %u on s%u\n", s, act, i);
			}

			switch(act) {
				case 0:
				case 1:
					sim_ready(i);
					break;
				case 2:
					sim_exit_svc(i, tap_below(2) ? 1 << 8 : SIGKILL);
					break;
				case 3:
					sim_say(i, tap_below(2) ? "line\n" : "partial");
					break;
				case 4: {
					char req[32];

					snprintf(req, sizeof(req), "%s s%u", verbs[tap_below(5)], i);
					if(strstr(req, "resume") || strstr(req, "status s")) {
						snprintf(req, sizeof(req), "%s", tap_below(2) ? "resume" : "status");
					}

					if(sim_trace) {
						printf("#   request %s\n", req);
					}

					sim_ctl(req);
					break;
				}

				case 5: {
					struct sim_child* c = kid_svc(i);

					if(c) {
						c->ignore_term = 1;
					}

					break;
				}

				default:
					sim_advance(tap_below(800));
			}

			trace_states();
			why = invariants();
			if(why) {
				tap_diag("graph %d (seed %#llx) step %d: %s", g, (unsigned long long)seed, s, why);
				bad = 1;
			}
		}
	}

	ok(!bad, "%d random graphs, %d random events each, keep the counters consistent", graphs, steps);
}

int main(void) {
	logcap_quiet();
	test_sim_fuzz();
	sim_reset();

	return tap_done();
}
