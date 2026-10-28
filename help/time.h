#pragma once

#include <time.h>

static inline long long mono_ms(void) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline long long mono_ms_since(const struct timespec* t0) {
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	return (long long)(now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

static inline long long ms_until(long long at, long long now) {
	return at > now ? at - now : 0;
}

// the sooner of two timeouts, a negative one is none
static inline long long ms_sooner(long long a, long long b) {
	return b >= 0 && (a < 0 || b < a) ? b : a;
}
