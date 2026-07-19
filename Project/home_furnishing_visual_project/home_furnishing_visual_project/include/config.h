#ifndef CONFIG_H
#define CONFIG_H

#include "project.h"

typedef enum SerialMode {
    SERIAL_RANDOM = 0,
    SERIAL_FROM_FILE = 1
} SerialMode;

typedef struct Config {
    int members_per_team;
    int furniture_count;
    int rounds_to_play;     /* scheduled rounds before tie-breakers; 0 derives from wins_to_end */
    int wins_to_end;        /* legacy setting: first-to target used to derive rounds_to_play */

    int min_delay_ms;
    int max_delay_ms;
    int fatigue_step_ms;
    int fatigue_every_moves;

    unsigned int random_seed;      /* 0 means use time + getpid */
    SerialMode serial_mode;
    char serial_file[MAX_PATH];

    int graphics_enabled;
    char graphics_fifo[MAX_PATH];
    char visualizer_path[MAX_PATH]; /* executable launched by master when graphics are enabled */

    char log_file[MAX_PATH];
    int round_pause_ms;
    int master_poll_ms;
    int max_round_seconds;         /* 0 means no timeout */
    int verbose;
} Config;

void config_set_defaults(Config *cfg);
int config_load_file(Config *cfg, const char *path);
void config_print(const Config *cfg, FILE *out);
int config_validate(const Config *cfg, char *error_buf, size_t error_len);

#endif
