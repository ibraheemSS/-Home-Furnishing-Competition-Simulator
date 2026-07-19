#ifndef UTIL_H
#define UTIL_H

#include "config.h"
#include "ipc.h"
#include <time.h>

long now_ms(void);
void sleep_ms(int ms);
void sleep_ms_interruptible(int ms);
int random_between(unsigned int *seed, int min_value, int max_value);
int tired_delay_ms(const Config *cfg, unsigned int *seed, int moves_done);
void trim_in_place(char *s);
int parse_bool_value(const char *value, int *out);
const char *role_name(int member_id, int members_per_team);
void safe_snprintf(char *dst, size_t dst_size, const char *fmt, ...);
int ensure_directory_for_file(const char *file_path);
int append_log_line(const char *file_path, const char *line);
void event_to_log_line(const EventMsg *ev, char *buf, size_t buf_size);

#endif
