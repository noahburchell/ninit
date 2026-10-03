#pragma once

extern int sfd;

void setup_signals(void);
void handle_signals(void);
void poll_signals(void);
void report_stalls(void);
void check_timeouts(void);
