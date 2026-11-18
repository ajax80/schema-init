#include "../seccomp_names.h"
#include "../seccomp_groups.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int idx_of(const char *n) {
    for (int i = 0; i < SC_NAMES_ALL; i++)
        if (!strcmp(sc_known[i], n)) return i;
    return -1;
}

static int in_group(const char *g, const char *n) {
    int want = idx_of(n);
    for (int k = 0; k < SC_GROUPS; k++) {
        if (strcmp(sc_groups[k].name, g)) continue;
        for (const unsigned short *p = sc_groups[k].idx; *p != 0xffff; p++)
            if (*p == want) return 1;
        return 0;
    }
    return -1;
}

int main(void) {
    assert(SC_NAMES_ALL > 400);
    for (int i = 1; i < SC_NAMES_ALL; i++) assert(strcmp(sc_known[i - 1], sc_known[i]) < 0);
    assert(sc_nr[idx_of("read")] == __NR_read && sc_nr[idx_of("execve")] == __NR_execve);
    assert(sc_nr[idx_of("openat")] == __NR_openat);
    assert(in_group("@system-service", "openat") == 1 && in_group("@system-service", "init_module") == 0);
    assert(in_group("@privileged", "setuid") == 1);
    assert(in_group("@mount", "mount") == 1 && in_group("@nonesuch", "mount") == -1);
    /* groups are pre-expanded: @system-service holds @basic-io's read */
    assert(in_group("@system-service", "read") == 1);
    for (int k = 0; k < SC_GROUPS; k++)
        for (const unsigned short *p = sc_groups[k].idx; *p != 0xffff; p++) assert(*p < SC_NAMES_ALL);
#if defined(__x86_64__)
    assert(sc_nr[idx_of("open")] == __NR_open);
#elif defined(__aarch64__)
    assert(sc_nr[idx_of("open")] == -1);
#endif
    printf("all seccomp table tests passed\n");
    return 0;
}
