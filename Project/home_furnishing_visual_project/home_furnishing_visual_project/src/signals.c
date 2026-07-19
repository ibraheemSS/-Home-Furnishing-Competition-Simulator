#include "signals.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

volatile sig_atomic_t g_round_active = 0;
volatile sig_atomic_t g_start_round_requested = 0;
volatile sig_atomic_t g_terminate_requested = 0;

static void child_handler(int signo) {
    if (signo == SIGUSR1) {
        g_start_round_requested = 1;
        g_round_active = 1;
    } else if (signo == SIGUSR2) {
        g_round_active = 0;
    } else if (signo == SIGTERM || signo == SIGINT) {
        g_terminate_requested = 1;
        g_round_active = 0;
    }
}

static void master_handler(int signo) {
    if (signo == SIGTERM || signo == SIGINT) {
        g_terminate_requested = 1;
    }
}

static int install_handler(int signo, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* Do not use SA_RESTART; poll/read should return on signals. */
    return sigaction(signo, &sa, NULL);
}

int install_child_signal_handlers(void) {
    if (install_handler(SIGUSR1, child_handler) != 0) return -1;
    if (install_handler(SIGUSR2, child_handler) != 0) return -1;
    if (install_handler(SIGTERM, child_handler) != 0) return -1;
    if (install_handler(SIGINT, child_handler) != 0) return -1;
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

int install_master_signal_handlers(void) {
    if (install_handler(SIGTERM, master_handler) != 0) return -1;
    if (install_handler(SIGINT, master_handler) != 0) return -1;
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void reset_child_signal_state(void) {
    g_round_active = 0;
    g_start_round_requested = 0;
    g_terminate_requested = 0;
}
