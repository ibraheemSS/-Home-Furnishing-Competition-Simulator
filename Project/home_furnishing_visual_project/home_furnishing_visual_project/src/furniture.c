#include "furniture.h"
#include "util.h"

#include <errno.h>

#ifdef USE_OPENMP
#include <omp.h>
#endif

static int load_serials_from_file(const Config *cfg, int *serials, char *error_buf, size_t error_len) {
    FILE *fp = fopen(cfg->serial_file, "r");
    if (!fp) {
        snprintf(error_buf, error_len, "cannot open serial_file '%s': %s", cfg->serial_file, strerror(errno));
        return -1;
    }

    int n = cfg->furniture_count;
    unsigned char *seen = calloc((size_t)n + 1, sizeof(unsigned char));
    if (!seen) {
        fclose(fp);
        snprintf(error_buf, error_len, "out of memory while reading serial file");
        return -1;
    }

    for (int i = 0; i < n; i++) {
        int v;
        if (fscanf(fp, "%d", &v) != 1) {
            snprintf(error_buf, error_len, "serial_file must contain %d integers", n);
            free(seen);
            fclose(fp);
            return -1;
        }
        if (v < 1 || v > n) {
            snprintf(error_buf, error_len, "serial number %d is outside valid range 1..%d", v, n);
            free(seen);
            fclose(fp);
            return -1;
        }
        if (seen[v]) {
            snprintf(error_buf, error_len, "duplicate serial number %d in serial_file", v);
            free(seen);
            fclose(fp);
            return -1;
        }
        seen[v] = 1;
        serials[i] = v;
    }

    int extra;
    if (fscanf(fp, "%d", &extra) == 1) {
        snprintf(error_buf, error_len, "serial_file contains more than %d integers", n);
        free(seen);
        fclose(fp);
        return -1;
    }

    free(seen);
    fclose(fp);
    return 0;
}

int generate_serials(const Config *cfg, int round_id, int *serials, char *error_buf, size_t error_len) {
    int n = cfg->furniture_count;
    if (cfg->serial_mode == SERIAL_FROM_FILE) {
        return load_serials_from_file(cfg, serials, error_buf, error_len);
    }

#ifdef USE_OPENMP
#pragma omp parallel for if(n > 5)
#endif
    for (int i = 0; i < n; i++) {
        serials[i] = i + 1;
    }

    unsigned int seed = cfg->random_seed + (unsigned int)(round_id * 2654435761u);
    if (seed == 0) seed = 1u;
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rand_r(&seed) % (unsigned int)(i + 1));
        int tmp = serials[i];
        serials[i] = serials[j];
        serials[j] = tmp;
    }
    (void)error_buf;
    (void)error_len;
    return 0;
}

int any_available_piece(int furniture_count, const unsigned char *delivered, const unsigned char *blocked) {
    for (int i = 0; i < furniture_count; i++) {
        if (!delivered[i] && !blocked[i]) return 1;
    }
    return 0;
}

int choose_available_piece(int furniture_count, const unsigned char *delivered,
                           const unsigned char *blocked, unsigned int *seed) {
    int count = 0;
    for (int i = 0; i < furniture_count; i++) {
        if (!delivered[i] && !blocked[i]) count++;
    }
    if (count == 0) return -1;

    int target = (int)(rand_r(seed) % (unsigned int)count);
    for (int i = 0; i < furniture_count; i++) {
        if (!delivered[i] && !blocked[i]) {
            if (target == 0) return i;
            target--;
        }
    }
    return -1;
}

void clear_blocks(int furniture_count, unsigned char *blocked) {
#ifdef USE_OPENMP
#pragma omp parallel for if(furniture_count > 5)
#endif
    for (int i = 0; i < furniture_count; i++) {
        blocked[i] = 0;
    }
}

void reset_status_arrays(int furniture_count, unsigned char *delivered, unsigned char *blocked) {
#ifdef USE_OPENMP
#pragma omp parallel for if(furniture_count > 5)
#endif
    for (int i = 0; i < furniture_count; i++) {
        delivered[i] = 0;
        blocked[i] = 0;
    }
}
