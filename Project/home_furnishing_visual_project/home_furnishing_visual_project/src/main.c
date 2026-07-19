#include "config.h"
#include "master.h"
#include "util.h"

#include <unistd.h>

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <config-file>\n", prog);
    fprintf(stderr, "Example: %s config/sample.conf\n", prog);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 2;
    }

    Config cfg;
    config_set_defaults(&cfg);
    if (config_load_file(&cfg, argv[1]) != 0) {
        return 2;
    }

    if (cfg.rounds_to_play <= 0) {
        cfg.rounds_to_play = cfg.wins_to_end > 0 ? (2 * cfg.wins_to_end - 1) : 1;
    }

    if (cfg.random_seed == 0) {
        cfg.random_seed = (unsigned int)(now_ms() ^ (long)getpid());
        if (cfg.random_seed == 0) cfg.random_seed = 1;
    }

    char error[MAX_TEXT];
    if (config_validate(&cfg, error, sizeof(error)) != 0) {
        fprintf(stderr, "Invalid configuration: %s\n", error);
        return 2;
    }

    config_print(&cfg, stdout);
    printf("\n");
    fflush(stdout);

    return run_master(&cfg);
}
