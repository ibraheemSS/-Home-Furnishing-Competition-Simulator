#ifndef SIGNALS_H
#define SIGNALS_H

#include "project.h"

extern volatile sig_atomic_t g_round_active;
extern volatile sig_atomic_t g_start_round_requested;
extern volatile sig_atomic_t g_terminate_requested;

int install_child_signal_handlers(void);
int install_master_signal_handlers(void);
void reset_child_signal_state(void);

#endif
