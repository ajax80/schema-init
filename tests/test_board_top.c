#include "../board_top.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static schema_shm_t z;
static top_sample_t sm[SCHEMA_SHM_MAX];

static void add(int i, const char *name, uint8_t st, double cpu, long long mem) {
    snprintf(z.svc[i].name, 64, "%s", name);
    z.svc[i].state = st;
    z.svc[i].child_pid = 100 + i;
    snprintf(sm[i].name, 64, "%s", name);
    sm[i].cpu = cpu;
    sm[i].mem = mem;
    sm[i].have = mem >= 0;
    sm[i].have_cpu = 1;
}

static void order_is(int key, int rev, const char *want) {
    int o[3];
    char got[16] = "";
    top_sort(&z, sm, o, 3, key, rev);
    for (int i = 0; i < 3; i++) strncat(got, z.svc[o[i]].name, 1);
    if (strcmp(got, want)) { fprintf(stderr, "sort key=%d rev=%d: %s want %s\n", key, rev, got, want); abort(); }
}

int main(void) {
    assert(top_cpu_pct(1000000, 1500000, 1000000, 9) == 50.0);
    assert(top_cpu_pct(0, 2000000, 1000000, 0) == 200.0);
    assert(top_cpu_pct(0, 999, 50000, 7.5) == 7.5);      /* under 100 ms: keep */
    assert(top_cpu_pct(5, 4, 1000000, 3) == 3);          /* counter went back */

    z.magic = SCHEMA_SHM_MAGIC;
    z.count = 3;
    add(0, "a", STATE_FUNDAMENTAL, 10, 300);
    add(1, "b", STATE_FUNDAMENTAL, 40, 100);
    add(2, "c", STATE_EXCISED, 40, 200);
    order_is(TOP_SORT_CPU, 0, "bca");
    order_is(TOP_SORT_CPU, 1, "acb");
    order_is(TOP_SORT_MEM, 0, "acb");
    order_is(TOP_SORT_NAME, 0, "abc");
    order_is(TOP_SORT_NAME, 1, "cba");

    sm[0].cpu = 0.0; sm[1].cpu = 60; sm[2].cpu = 150;
    int o[3] = { 0, 1, 2 };
    char *buf = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    top_render(f, &z, sm, o, 3, TOP_SORT_CPU, 0, 0, 20, 80);
    fclose(f);
    assert(strstr(buf, "\033[2m  0.0"));
    assert(strstr(buf, "\033[33m 60.0"));
    assert(strstr(buf, "\033[1;31m150.0"));
    assert(strstr(buf, "sort: CPU"));
    assert(strstr(buf, "\033[1m CPU%"));
    assert(strstr(buf, "2 ok"));
    assert(strstr(buf, "1 excised"));
    assert(strstr(buf, "mem 600B"));
    free(buf);
    puts("test_board_top: ok");
    return 0;
}
