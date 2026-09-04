#pragma once

#include <stddef.h>

#define LOG_DONE	0
#define LOG_INFO	1
#define LOG_WARN	2
#define LOG_FAIL	3
#define LOG_WAIT	4
#define LOG_NOTE	5
#define LOG_N		6

void log_init(void);
void log_reopen_console(void);
void log_adopt_fd(int fd);
void ninit_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_raw(int level, const char *buf, size_t len);
void print_welcome(void);

#ifdef NINIT_QUIET
#define log_done(...)	((void)0)
#define log_info(...)	((void)0)
#define log_wait(...)	((void)0)
#define log_note(...)	((void)0)
#else
#define log_done(...)	ninit_log(LOG_DONE, __VA_ARGS__)
#define log_info(...)	ninit_log(LOG_INFO, __VA_ARGS__)
#define log_wait(...)	ninit_log(LOG_WAIT, __VA_ARGS__)
#define log_note(...)	ninit_log(LOG_NOTE, __VA_ARGS__)
#endif

#define log_warn(...)	ninit_log(LOG_WARN, __VA_ARGS__)
#define log_err(...)	ninit_log(LOG_FAIL, __VA_ARGS__)
