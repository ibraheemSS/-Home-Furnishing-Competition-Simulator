#ifndef FURNITURE_H
#define FURNITURE_H

#include "config.h"

int generate_serials(const Config *cfg, int round_id, int *serials, char *error_buf, size_t error_len);
int choose_available_piece(int furniture_count, const unsigned char *delivered,
                           const unsigned char *blocked, unsigned int *seed);
int any_available_piece(int furniture_count, const unsigned char *delivered,
                        const unsigned char *blocked);
void clear_blocks(int furniture_count, unsigned char *blocked);
void reset_status_arrays(int furniture_count, unsigned char *delivered, unsigned char *blocked);

#endif
