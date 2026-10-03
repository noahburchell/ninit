#include "signals.h"
#include "drain.h"
#include "fail.h"
#include "logging.h"
#include "ngraph.h"
#include "ninit.h"
#include "reap.h"
#include "stop.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int sfd = -1;
static sigset_t sig_want;

void setup_signals(void)
{
	sigset_t all;

	sigfillset(&all);

	sigdelset(&all, SIGSEGV);
	sigdelset(&all, SIGBUS);
	sigdelset(&all, SIGILL);
	sigdelset(&all, SIGFPE);
	sigprocmask(SIG_SETMASK, &all, NULL);

	sigemptyset(&sig_want);
	sigaddset(&sig_want, SIGCHLD);
	sigaddset(&sig_want, SIGTERM);
	sigaddset(&sig_want, SIGINT);
	sigaddset(&sig_want, SIGUSR1);
	sigaddset(&sig_want, SIGUSR2);
	sfd = signalfd(-1, &sig_want, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd < 0)
		log_err("signalfd: %s, polling for signals instead", strerror(errno));

	reboot(RB_DISABLE_CAD);
}

static void dispatch_signal(int signo)
{
	switch (signo) {
	case SIGCHLD:
		reap();
		break;
	case SIGTERM:
	case SIGINT:
		shutdown_system(RB_AUTOBOOT, "reboot");
	case SIGUSR1:
		shutdown_system(RB_HALT_SYSTEM, "halt");
	case SIGUSR2:
		shutdown_system(RB_POWER_OFF, "poweroff");
	default:
		break;
	}
}

void handle_signals(void)
{
	struct signalfd_siginfo si;

	while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si))
		dispatch_signal((int)si.ssi_signo);
}

void poll_signals(void)
{
	static const struct timespec zero = { 0, 0 };
	siginfo_t si;

	sfd = signalfd(-1, &sig_want, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd >= 0)
		log_note("signalfd: recovered");

	while (sigtimedwait(&sig_want, &si, &zero) > 0)
		dispatch_signal(si.si_signo);
}

void report_stalls(void)
{
	uint32_t k;

	for (k = 0; k < n_live; k++) {
		uint32_t i = live[k];

		if (runs[i].starting)
			log_wait("%s: still running after %lld s", ng_name(map, i),
				 (now_ms() - runs[i].started) / 1000);
	}
}

void check_timeouts(void)
{
	long long now = now_ms();
	uint32_t k;

	if (shutting_down)
		return;

	for (k = 0; k < n_live; ) {
		uint32_t i = live[k];
		struct run *r = &runs[i];

		if (!r->starting || r->timedout ||
		    now - r->started < (long long)ng_start_ms(map, i)) {
			k++;
			continue;
		}

		if (r->ntf_fd >= 0) {
			uint32_t before = n_live;

			if (r->ntf_exec)
				drain_exec(i);
			else
				drain_notify(i, DRAIN_FINAL_CHUNKS);
			if (!r->starting || r->timedout || n_live != before) {
				k = 0;
				continue;
			}
		}

		log_err("%s: start timed out after %u ms", ng_name(map, i), ng_start_ms(map, i));
		r->timedout = 1;
		if (state[i] == NG_ST_RUNNING)
			service_failed(i, FAIL_ST_TIMEOUT);
		else
			start_failed(i, FAIL_ST_TIMEOUT);
		k = 0;
	}
}
