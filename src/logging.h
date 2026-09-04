#pragma once

#include <stddef.h>

#define LOG_OK		0
#define LOG_INFO	1
#define LOG_WARN	2
#define LOG_ERR		3

void log_init(void);
void log_reopen_console(void);
void ninit_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_raw(int level, const char *buf, size_t len);
void print_welcome(void);

#ifdef NINIT_QUIET
#define log_ok(...)	((void)0)
#define log_info(...)	((void)0)
#else
#define log_ok(...)	ninit_log(LOG_OK, __VA_ARGS__)
#define log_info(...)	ninit_log(LOG_INFO, __VA_ARGS__)
#endif

#define log_warn(...)	ninit_log(LOG_WARN, __VA_ARGS__)
#define log_err(...)	ninit_log(LOG_ERR, __VA_ARGS__)
