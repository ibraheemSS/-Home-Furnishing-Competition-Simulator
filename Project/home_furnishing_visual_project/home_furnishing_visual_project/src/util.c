#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        continue;
    }
}

void sleep_ms_interruptible(int ms) {
    if (ms <= 0) return;
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

int random_between(unsigned int *seed, int min_value, int max_value) {
    if (max_value <= min_value) return min_value;
    unsigned int r = rand_r(seed);
    int width = max_value - min_value + 1;
    return min_value + (int)(r % (unsigned int)width);
}

int tired_delay_ms(const Config *cfg, unsigned int *seed, int moves_done) {
    int base = random_between(seed, cfg->min_delay_ms, cfg->max_delay_ms);
    int fatigue = 0;
    if (cfg->fatigue_every_moves > 0) {
        fatigue = (moves_done / cfg->fatigue_every_moves) * cfg->fatigue_step_ms;
    }
    return base + fatigue;
}

void trim_in_place(char *s) {
    if (!s) return;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

int parse_bool_value(const char *value, int *out) {
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0 || strcmp(value, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

const char *role_name(int member_id, int members_per_team) {
    if (member_id == 0) return "source";
    if (member_id == members_per_team - 1) return "sink";
    return "middle";
}

void safe_snprintf(char *dst, size_t dst_size, const char *fmt, ...) {
    if (!dst || dst_size == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, dst_size, fmt, ap);
    va_end(ap);
    dst[dst_size - 1] = '\0';
}

static int mkdir_p(const char *dir) {
    if (!dir || dir[0] == '\0') return 0;
    char tmp[MAX_PATH * 2];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) == -1 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0775) == -1 && errno != EEXIST) return -1;
    return 0;
}

int ensure_directory_for_file(const char *file_path) {
    if (!file_path || file_path[0] == '\0') return 0;
    char copy[MAX_PATH * 2];
    snprintf(copy, sizeof(copy), "%s", file_path);
    char *slash = strrchr(copy, '/');
    if (!slash) return 0;
    *slash = '\0';
    if (copy[0] == '\0') return 0;
    return mkdir_p(copy);
}

int append_log_line(const char *file_path, const char *line) {
    if (!file_path || file_path[0] == '\0') return 0;
    if (ensure_directory_for_file(file_path) != 0) return -1;
    FILE *fp = fopen(file_path, "a");
    if (!fp) return -1;
    fprintf(fp, "%s\n", line);
    fclose(fp);
    return 0;
}

void event_to_log_line(const EventMsg *ev, char *buf, size_t buf_size) {
    const char *type = "NONE";
    switch (ev->type) {
        case EVENT_LOG: type = "LOG"; break;
        case EVENT_ACCEPTED: type = "ACCEPTED"; break;
        case EVENT_REJECTED: type = "REJECTED"; break;
        case EVENT_ROUND_WIN: type = "ROUND_WIN"; break;
        case EVENT_CHILD_READY: type = "CHILD_READY"; break;
        case EVENT_CHILD_EXIT: type = "CHILD_EXIT"; break;
        case EVENT_ERROR: type = "ERROR"; break;
        case EVENT_MOVED: type = "MOVED"; break;
        case EVENT_VISUAL_RESULT: type = "VISUAL_RESULT"; break;
        case EVENT_ROUND_START: type = "ROUND_START"; break;
        case EVENT_TIE_BREAK: type = "TIE_BREAK"; break;
        case EVENT_COMPETITION_END: type = "COMPETITION_END"; break;
        case EVENT_ROUND_RESULT: type = "ROUND_RESULT"; break;
        default: break;
    }
    snprintf(buf, buf_size,
             "%ld type=%s team=%d member=%d round=%d piece=%d serial=%d accepted=%d/%d expected=%d "
             "wins=%d:%d members=%d dir=%d from=%d to=%d touches=%d text=%s",
             ev->timestamp_ms, type, ev->team_id, ev->member_id, ev->round_id,
             ev->piece_id, ev->serial_no, ev->accepted_count, ev->furniture_count,
             ev->expected_serial, ev->wins[0], ev->wins[1], ev->member_count,
             ev->direction, ev->from_member, ev->to_member, ev->touches, ev->text);
}
