#pragma once

#include <stddef.h>
#include <stdint.h>

#define LOG_DONE	0
#define LOG_INFO	1
#define LOG_WARN	2
#define LOG_FAIL	3
#define LOG_WAIT	4
#define LOG_NOTE	5
#define LOG_N		6
#define LOG_NOCON	0x10

#define LOG_LINE	1024
#define LOG_RING	(128u << 10)

void log_init(void);
void log_batch_begin(void);
void log_reopen_console(void);
void log_adopt_fd(int fd);
void ninit_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_raw(int level, const char *buf, size_t len);
void print_welcome(void);

uint64_t log_first(void);
uint64_t log_end(void);
size_t log_line(uint64_t *at, char *buf);
void log_feed(void (*fn)(void));
int log_pending_fd(void);
void log_flush(void);

// a quiet build keeps these in the ring but off the console
#ifdef NINIT_QUIET
#define LOG_QUIET	LOG_NOCON
#else
#define LOG_QUIET	0
#endif

#define log_done(...)	ninit_log(LOG_DONE | LOG_QUIET, __VA_ARGS__)
#define log_info(...)	ninit_log(LOG_INFO | LOG_QUIET, __VA_ARGS__)
#define log_wait(...)	ninit_log(LOG_WAIT | LOG_QUIET, __VA_ARGS__)
#define log_note(...)	ninit_log(LOG_NOTE | LOG_QUIET, __VA_ARGS__)

#define log_warn(...)	ninit_log(LOG_WARN, __VA_ARGS__)
#define log_err(...)	ninit_log(LOG_FAIL, __VA_ARGS__)
