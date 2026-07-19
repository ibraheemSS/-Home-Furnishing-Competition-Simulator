#include "config.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>

static int parse_int(const char *value, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(value, &end, 10);
    if (errno != 0 || end == value) return -1;
    while (*end) {
        if (!isspace((unsigned char)*end)) return -1;
        end++;
    }
    if (v < INT_MIN || v > INT_MAX) return -1;
    *out = (int)v;
    return 0;
}

static int parse_uint(const char *value, unsigned int *out) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(value, &end, 10);
    if (errno != 0 || end == value) return -1;
    while (*end) {
        if (!isspace((unsigned char)*end)) return -1;
        end++;
    }
    if (v > UINT_MAX) return -1;
    *out = (unsigned int)v;
    return 0;
}

void config_set_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->members_per_team = 5;
    cfg->furniture_count = 30;
    cfg->rounds_to_play = 0;
    cfg->wins_to_end = 3;

    cfg->min_delay_ms = 20;
    cfg->max_delay_ms = 100;
    cfg->fatigue_step_ms = 3;
    cfg->fatigue_every_moves = 30;

    cfg->random_seed = 0;
    cfg->serial_mode = SERIAL_RANDOM;
    cfg->serial_file[0] = '\0';

    cfg->graphics_enabled = 0;
    snprintf(cfg->graphics_fifo, sizeof(cfg->graphics_fifo), "/tmp/home_furnishing_fifo");
    snprintf(cfg->visualizer_path, sizeof(cfg->visualizer_path), "bin/visualizer");

    snprintf(cfg->log_file, sizeof(cfg->log_file), "logs/simulation.log");
    cfg->round_pause_ms = 2000;
    cfg->master_poll_ms = 200;
    cfg->max_round_seconds = 0;
    cfg->verbose = 1;
}

int config_load_file(Config *cfg, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Could not open config file '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        trim_in_place(line);
        if (line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "Config error %s:%d: expected key=value\n", path, line_no);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim_in_place(key);
        trim_in_place(value);

        if (strcmp(key, "members_per_team") == 0) {
            if (parse_int(value, &cfg->members_per_team) != 0) goto bad_value;
        } else if (strcmp(key, "furniture_count") == 0) {
            if (parse_int(value, &cfg->furniture_count) != 0) goto bad_value;
        } else if (strcmp(key, "rounds_to_play") == 0 || strcmp(key, "total_rounds") == 0) {
            if (parse_int(value, &cfg->rounds_to_play) != 0) goto bad_value;
        } else if (strcmp(key, "wins_to_end") == 0) {
            if (parse_int(value, &cfg->wins_to_end) != 0) goto bad_value;
        } else if (strcmp(key, "min_delay_ms") == 0) {
            if (parse_int(value, &cfg->min_delay_ms) != 0) goto bad_value;
        } else if (strcmp(key, "max_delay_ms") == 0) {
            if (parse_int(value, &cfg->max_delay_ms) != 0) goto bad_value;
        } else if (strcmp(key, "fatigue_step_ms") == 0) {
            if (parse_int(value, &cfg->fatigue_step_ms) != 0) goto bad_value;
        } else if (strcmp(key, "fatigue_every_moves") == 0) {
            if (parse_int(value, &cfg->fatigue_every_moves) != 0) goto bad_value;
        } else if (strcmp(key, "random_seed") == 0) {
            if (parse_uint(value, &cfg->random_seed) != 0) goto bad_value;
        } else if (strcmp(key, "serial_mode") == 0) {
            if (strcmp(value, "random") == 0) cfg->serial_mode = SERIAL_RANDOM;
            else if (strcmp(value, "file") == 0) cfg->serial_mode = SERIAL_FROM_FILE;
            else goto bad_value;
        } else if (strcmp(key, "serial_file") == 0) {
            snprintf(cfg->serial_file, sizeof(cfg->serial_file), "%s", value);
        } else if (strcmp(key, "graphics_enabled") == 0) {
            if (parse_bool_value(value, &cfg->graphics_enabled) != 0) goto bad_value;
        } else if (strcmp(key, "graphics_fifo") == 0) {
            snprintf(cfg->graphics_fifo, sizeof(cfg->graphics_fifo), "%s", value);
        } else if (strcmp(key, "visualizer_path") == 0) {
            snprintf(cfg->visualizer_path, sizeof(cfg->visualizer_path), "%s", value);
        } else if (strcmp(key, "log_file") == 0) {
            snprintf(cfg->log_file, sizeof(cfg->log_file), "%s", value);
        } else if (strcmp(key, "round_pause_ms") == 0) {
            if (parse_int(value, &cfg->round_pause_ms) != 0) goto bad_value;
        } else if (strcmp(key, "master_poll_ms") == 0) {
            if (parse_int(value, &cfg->master_poll_ms) != 0) goto bad_value;
        } else if (strcmp(key, "max_round_seconds") == 0) {
            if (parse_int(value, &cfg->max_round_seconds) != 0) goto bad_value;
        } else if (strcmp(key, "verbose") == 0) {
            if (parse_bool_value(value, &cfg->verbose) != 0) goto bad_value;
        } else {
            fprintf(stderr, "Config warning %s:%d: unknown key '%s' ignored\n", path, line_no, key);
        }
        continue;

bad_value:
        fprintf(stderr, "Config error %s:%d: bad value for key '%s': '%s'\n", path, line_no, key, value);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

int config_validate(const Config *cfg, char *error_buf, size_t error_len) {
#define FAIL(...) do { snprintf(error_buf, error_len, __VA_ARGS__); return -1; } while (0)
    if (cfg->members_per_team < 2) FAIL("members_per_team must be at least 2");
    if (cfg->members_per_team > MAX_MEMBERS) FAIL("members_per_team must be <= %d", MAX_MEMBERS);
    if (cfg->furniture_count < 1) FAIL("furniture_count must be positive");
    if (cfg->rounds_to_play < 1) FAIL("rounds_to_play must be positive");
    if (cfg->wins_to_end < 1) FAIL("wins_to_end must be positive");
    if (cfg->min_delay_ms < 0 || cfg->max_delay_ms < 0) FAIL("delay values must be non-negative");
    if (cfg->min_delay_ms > cfg->max_delay_ms) FAIL("min_delay_ms cannot be greater than max_delay_ms");
    if (cfg->fatigue_step_ms < 0) FAIL("fatigue_step_ms must be non-negative");
    if (cfg->fatigue_every_moves < 0) FAIL("fatigue_every_moves must be non-negative");
    if (cfg->serial_mode == SERIAL_FROM_FILE && cfg->serial_file[0] == '\0') {
        FAIL("serial_mode=file requires serial_file");
    }
    if (cfg->graphics_enabled && cfg->visualizer_path[0] == '\0') FAIL("graphics_enabled requires visualizer_path");
    if (cfg->round_pause_ms < 0) FAIL("round_pause_ms must be non-negative");
    if (cfg->master_poll_ms < 10) FAIL("master_poll_ms must be at least 10");
    if (cfg->max_round_seconds < 0) FAIL("max_round_seconds must be >= 0");
    return 0;
#undef FAIL
}

void config_print(const Config *cfg, FILE *out) {
    fprintf(out, "Configuration:\n");
    fprintf(out, "  members_per_team    = %d\n", cfg->members_per_team);
    fprintf(out, "  furniture_count     = %d\n", cfg->furniture_count);
    fprintf(out, "  rounds_to_play      = %d\n", cfg->rounds_to_play);
    fprintf(out, "  wins_to_end         = %d (legacy first-to setting)\n", cfg->wins_to_end);
    fprintf(out, "  delay range         = %d..%d ms\n", cfg->min_delay_ms, cfg->max_delay_ms);
    fprintf(out, "  fatigue             = +%d ms every %d moves\n", cfg->fatigue_step_ms, cfg->fatigue_every_moves);
    fprintf(out, "  random_seed         = %u%s\n", cfg->random_seed, cfg->random_seed == 0 ? " (auto)" : "");
    fprintf(out, "  serial_mode         = %s\n", cfg->serial_mode == SERIAL_RANDOM ? "random" : "file");
    if (cfg->serial_mode == SERIAL_FROM_FILE) fprintf(out, "  serial_file         = %s\n", cfg->serial_file);
    fprintf(out, "  graphics_enabled    = %s\n", cfg->graphics_enabled ? "yes" : "no");
    fprintf(out, "  graphics_fifo       = %s\n", cfg->graphics_fifo);
    fprintf(out, "  visualizer_path     = %s\n", cfg->visualizer_path);
    fprintf(out, "  log_file            = %s\n", cfg->log_file);
    fprintf(out, "  max_round_seconds   = %d\n", cfg->max_round_seconds);
}
