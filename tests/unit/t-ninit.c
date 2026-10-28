#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sim.h"

static void test_put(void) {
	char b[64];
	size_t n;

	n = put_num(b, 0, 0);
	is_int(n, 1, "put_num 0 is one digit");
	b[n] = '\0';
	is_str(b, "0", "put_num 0");
	n = put_num(b, 0, 4294967295u);
	b[n] = '\0';
	is_str(b, "4294967295", "put_num of the largest unsigned");
	n = put_str(b, 0, "errno ");
	n = put_num(b, n, 13);
	b[n] = '\0';
	is_str(b, "errno 13", "put_str then put_num");
}

static void test_unescape(void) {
	static const struct {
		const char* in;
		const char* out;
	} v[] = {
		{ "/mnt/a\\040b", "/mnt/a b" },
		{ "/x\\011y", "/x\ty" },
		{ "/back\\134slash", "/back\\slash" },
		{ "/nl\\012", "/nl\n" },
		{ "/two\\040\\040", "/two  " },
		{ "/bad\\8", "/bad\\8" },
		{ "/short\\04", "/short\\04" },
		{ "/end\\", "/end\\" },
		{ "/plain", "/plain" },
		{ "", "" },
	};

	for(size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		char buf[64], what[96];

		snprintf(buf, sizeof(buf), "%s", v[k].in);
		unescape_mount(buf);
		snprintf(what, sizeof(what), "unescape_mount case %zu", k);
		is_str(buf, v[k].out, what);
	}
}

static void test_rtc_local(void) {
	static const struct {
		const char* text;
		int local;
	} v[] = {
		{ "0.0 0 0.0\n0\nLOCAL\n", 1 },
		{ "0.0 0 0.0\n0\nLOCAL", 1 },
		{ "0.0 0 0.0\n0\nUTC\n", 0 },
		{ "0.0 0 0.0\n0\nLOCALTIME\n", 0 },
		{ "0.0 0 0.0\n0\nlocal\n", 0 },
		{ "0.0 0 0.0\nLOCAL\n", 0 },
		{ "LOCAL\n", 0 },
		{ "", 0 },
	};
	char path[] = "/tmp/ninit-t-ninit-adjtime.XXXXXX";
	int fd = mkstemp(path);

	close(fd);
	adjtime_path = path;
	for(size_t k = 0; k < sizeof(v) / sizeof(*v); k++) {
		fd = open(path, O_WRONLY | O_TRUNC);
		(void)!write(fd, v[k].text, strlen(v[k].text));
		close(fd);
		ok(rtc_local() == v[k].local, "adjtime case %zu is %s", k, v[k].local ? "LOCAL" : "UTC");
	}

	unlink(path);
	adjtime_path = NULL;
	ok(rtc_local() == 0, "without /etc/adjtime the clock is UTC");
}

// random insertions and deletions against a plain array model
static void test_pid_table(void) {
	pid_t model[64];
	uint32_t mval[64];
	int nm = 0, bad = 0, it;

	pid_mask = 63;
	pid_key = calloc(64, sizeof(*pid_key));
	pid_val = calloc(64, sizeof(*pid_val));
	runs = calloc(64, sizeof(*runs));
	n_live = 0;

	for(it = 0; it < 200000 && !bad; it++) {
		if(nm < 40 && (nm == 0 || tap_below(3))) {
			// pids that collide in the low bits stress the probing
			pid_t p = (pid_t)(1 + tap_below(4) * 64 + tap_below(8) * 4096 + tap_below(200));
			int dup = 0;

			for(int k = 0; k < nm; k++) {
				dup |= model[k] == p;
			}

			if(dup) {
				continue;
			}

			model[nm] = p;
			mval[nm] = tap_below(64);
			runs[mval[nm]].pid = p;
			pid_put(p, mval[nm]);
			nm++;
		} else {
			int k = (int)tap_below((uint32_t)nm);

			pid_del(model[k]);
			model[k] = model[--nm];
			mval[k] = mval[nm];
		}

		for(int k = 0; k < nm; k++) {
			int stale;
			uint32_t got;

			runs[mval[k]].pid = model[k];
			got = find_pid(model[k], &stale);
			if(got != mval[k]) {
				tap_diag("iteration %d: pid %d maps to %u, want %u", it, (int)model[k], got,
					 mval[k]);
				bad = 1;
				break;
			}
		}

		{
			int stale;
			pid_t absent = (pid_t)(900000 + tap_below(1000));

			if(find_pid(absent, &stale) != UINT32_MAX) {
				tap_diag("iteration %d: absent pid %d found", it, (int)absent);
				bad = 1;
			}
		}
	}

	ok(!bad, "the pid table agrees with a model over 200000 random operations");
	{
		uint32_t used = 0;

		for(uint32_t k = 0; k <= pid_mask; k++) {
			used += pid_key[k] != 0;
		}

		is_int(used, nm, "the table holds exactly the live entries");
	}

	free(pid_key);
	free(pid_val);
	free(runs);
	pid_key = NULL;
	pid_val = NULL;
	runs = NULL;
}

static void test_sim_basic(void) {
	static const struct gb_svc sv[] = {
		GB_ONESHOT("a", "x"),
		{ .name = "d", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .notify = 3, .script = "x" },
		GB_ONESHOT("b", "x"),
		GB_TARGET("t"),
		GB_ONESHOT("late", "x"),
	};
	static const struct gb_edge e[] = { { 0, 2 }, { 1, 3 }, { 2, 3 }, { 3, 4 } };

	sim_load(sv, 5, e, 4);
	ok(inv_ok("after start_graph"), "boot starts with consistent counters");
	is_str(st_of("a"), "starting", "a root oneshot is starting");
	is_str(st_of("d"), "starting", "a root daemon is starting");
	is_str(st_of("b"), "pending", "a dependent is pending");
	sim_ready(svc_by("a"));
	is_str(st_of("a"), "up", "a oneshot that exits 0 is up");
	is_str(st_of("b"), "starting", "its dependent starts at once");
	sim_ready(svc_by("b"));
	is_str(st_of("t"), "pending", "a target waits for every prerequisite");
	sim_say(svc_by("d"), "part");
	is_str(st_of("d"), "starting", "notify bytes without a newline are not readiness");
	sim_ready(svc_by("d"));
	is_str(st_of("t"), "up", "a target is up once its prerequisites are");
	is_str(st_of("late"), "starting", "and releases its dependents");
	sim_ready(svc_by("late"));
	sim_advance(10);
	ok(boot_ms >= 0, "the boot summary is printed once nothing is starting");
	ok(inv_ok("after boot"), "the counters agree after boot");
	is_int(n_up, 5, "all five services are up");
}

static void test_sim_restart_backoff(void) {
	static const struct gb_svc sv[] = {
		{ .name = "r", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .restart = 1, .notify = 3, .script = "x", .pol = { .start_ms = 300 } },
	};
	static const struct gb_svc slow[] = {
		{ .name = "slow", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .restart = 1, .notify = 3, .script = "x", .pol = { .start_ms = 12000 } },
	};
	long long expect[] = { 100, 250, 500, 1000, 2000, 5000, 5000 };
	uint32_t i;
	int bad = 0;

	sim_load(sv, 1, NULL, 0);
	i = svc_by("r");
	sim_ready(i);
	for(size_t k = 0; k < sizeof(expect) / sizeof(*expect); k++) {
		long long t0;

		sim_exit_svc(i, 1 << 8);
		t0 = fake_ms;
		if(runs[i].restart_at - t0 != expect[k] && bad++ < 3) {
			tap_diag("exit %zu: restart in %lld ms, want %lld", k, runs[i].restart_at - t0, expect[k]);
		}

		sim_advance(expect[k] + 50);
		sim_ready(i);
	}

	ok(!bad, "restart delays run 100, 250, 500, 1000, 2000, 5000, 5000 ms");
	sim_advance(10000);
	sim_exit_svc(i, 1 << 8);
	is_int(runs[i].restart_at - fake_ms, 100, "10 s up resets the delay to 100 ms");
	ok(inv_ok("restart"), "the counters agree through restarts");

	// a respawn that never becomes ready is killed at its start-timeout, then waits 250 ms
	sim_advance(150);
	is_str(st_of("r"), "down", "a respawning daemon is down until ready");
	{
		int forks = n_kids;
		long long t_kill = 0, t_spawn = 0;

		for(int k = 0; k < 200 && !t_spawn; k++) {
			fake_ms += 5;
			sim_step();
			if(!t_kill && runs[i].timedout) {
				t_kill = fake_ms;
			}

			if(t_kill && n_kids > forks + 0 && runs[i].pid > 0 && !runs[i].timedout) {
				t_spawn = fake_ms;
			}
		}

		ok(t_kill && t_spawn && t_spawn - t_kill >= 250,
		   "a respawn killed at its timeout waits its 250 ms backoff (respawned after %lld ms)",
		   t_spawn - t_kill);
	}

	ok(inv_ok("timeout respawn"), "the counters agree after timed out respawns");

	// a respawn that is never ready was never up, 12 s of starting does not reset the delay
	sim_load(slow, 1, NULL, 0);
	i = svc_by("slow");
	sim_ready(i);
	sim_exit_svc(i, 1 << 8);
	sim_advance(150);
	logcap_begin();
	for(int k = 0; k < 400 && !runs[i].restart_at; k++) {
		sim_advance(50);
	}

	is_int(runs[i].restart_at - fake_ms, 250, "a respawn killed at its start-timeout of 12 s keeps its delay");
	ok(logcap_count("slow: exited (timed out and was killed) after ") == 1
		   && logcap_count("ms, before it became ready, restarting in 250 ms") == 1 && !logcap_count("ms up"),
	   "and is not reported as up");
}

static void test_sim_ctl(void) {
	static const struct gb_svc sv[] = {
		{ .name = "d", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .script = "x", .pol = { .stop_ms = 200 } },
		GB_ONESHOT("o", "x"),
		GB_TARGET("t"),
	};
	static const struct gb_edge e[] = { { 0, 2 } };
	const char* r;
	uint32_t d;

	sim_load(sv, 3, e, 1);
	d = svc_by("d");
	sim_ready(d);
	sim_ready(svc_by("o"));
	r = sim_ctl("status");
	has_str(r, ". d up wanted pid ", "status lists d");
	has_str(r, "+ 3 services, 3 up\n", "status ends with the count");
	r = sim_ctl("stop d");
	is_str(r, "+ d stopped\n", "stop of a daemon that exits on SIGTERM");
	is_str(st_of("d"), "down", "it is down");
	is_str(st_of("t"), "down", "and so is the target over it");
	r = sim_ctl("stop d");
	is_str(r, "+ d already stopped\n", "stopping it again");
	r = sim_ctl("start d");
	is_str(r, "+ d up\n", "start brings it back once its shell execs");
	{
		struct sim_child* c = kid_svc(d);

		if(c) {
			c->ignore_term = 1;
		}

		ok(c != NULL, "the restarted daemon has a process");
	}

	r = sim_ctl("stop d");
	is_str(r, "+ d stopped after SIGKILL\n", "a daemon ignoring SIGTERM is killed after stop-timeout");
	r = sim_ctl("start t");
	is_str(r, "- t is a target, it has no process\n", "a target cannot be started");
	r = sim_ctl("start zz");
	is_str(r, "- no service named zz\n", "an unknown name is refused");
	r = sim_ctl("frob");
	is_str(r, "- unknown command 'frob'\n", "an unknown verb is refused");
	r = sim_ctl("resume");
	is_str(r, "+ nothing to resume\n", "resume with nothing failed");
	ok(inv_ok("ctl"), "the counters agree after control operations");
}

// a refused start must not leave a service counted up while it is listed pending
static void test_sim_refused(void) {
	static const struct gb_svc sv[] = {
		{ .name = "p", .type = NG_TYPE_DAEMON, .onfail = GB_DEFAULT, .script = "x" },
		GB_ONESHOT("o", "x"),
		GB_TARGET("t"),
	};
	static const struct gb_edge e[] = { { 0, 1 }, { 1, 2 } };
	const char* r;

	sim_load(sv, 3, e, 2);
	sim_step();
	sim_ready(svc_by("o"));
	is_str(st_of("o"), "up", "o is up");
	is_str(st_of("t"), "up", "t is up");
	sim_ctl("stop p");
	r = sim_ctl("start o");
	is_str(r, "- o waiting on a prerequisite\n", "start of o is refused while p is stopped");
	is_str(st_of("o"), "pending", "o is left pending");
	r = sim_ctl("status");
	{
		uint32_t rows = 0;
		const char* p = r;
		char expect[64];

		while((p = strstr(p, " up ")) != NULL) {
			rows++;
			p++;
		}

		snprintf(expect, sizeof(expect), "+ 3 services, %u up\n", rows);
		has_str(r, expect, "the up count matches the services listed up");
	}

	is_str(st_of("t"), "down", "a target over a pending prerequisite is down");
}

int main(void) {
	logcap_quiet();
	test_put();
	test_unescape();
	test_rtc_local();
	test_pid_table();
	test_sim_basic();
	test_sim_restart_backoff();
	test_sim_ctl();
	test_sim_refused();
	sim_reset();

	return tap_done();
}
