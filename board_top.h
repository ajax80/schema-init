#ifndef BOARD_TOP_H
#define BOARD_TOP_H
/* schema-top: schema-board's top view — the shm table plus per-service CPU% and
 * memory from each service's cgroup. Read-only; never touches the control socket. */
#include "schema.h"
#include "schema_shm.h"
#include <stdint.h>
#include <stdio.h>

enum { TOP_SORT_CPU, TOP_SORT_MEM, TOP_SORT_NAME };

typedef struct {
    char name[64];
    uint64_t usage_usec;
    int64_t mono_us;
    double cpu;
    long long mem;
    int have;        /* the cgroup was readable this round */
    int have_cpu;    /* a CPU% exists: two samples, far enough apart */
} top_sample_t;

double top_cpu_pct(uint64_t prev_usec, uint64_t cur_usec, int64_t elapsed_us, double keep);
int top_read_cgroup(const char *cgroot, const char *svc, uint64_t *usage_usec, long long *mem);
void top_sample_all(const schema_shm_t *s, top_sample_t *samp, int n, const char *cgroot);
void top_sort(const schema_shm_t *s, const top_sample_t *samp, int *order, int n, int key, int reverse);
void top_render(FILE *out, const schema_shm_t *s, const top_sample_t *samp, const int *order, int n,
                int key, int reverse, int first, int rows, int cols);
int top_main(int argc, char **argv);
#endif
