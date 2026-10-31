#pragma once

#include <stdint.h>
#include <time.h>

static inline int64_t mono_ms(void) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline int64_t mono_ms_since(const struct timespec* t0) {
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	return (int64_t)(now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

static inline int64_t ms_until(int64_t at, int64_t now) {
	return at > now ? at - now : 0;
}

// the sooner of two timeouts, a negative one is none
static inline int64_t ms_sooner(int64_t a, int64_t b) {
	return b >= 0 && (a < 0 || b < a) ? b : a;
}
