#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static service_t t[MAX_SERVICES];

static void reset(int n) {
    memset(t, 0, sizeof t);
    for (int i = 0; i < n; i++) {
        snprintf(t[i].name, sizeof t[i].name, "s%d", i);
        for (int d = 0; d < MAX_DEPS; d++) t[i].dep_idx[d] = -1;
    }
}

static void dep(int from, int to) {
    for (int d = 0; d < MAX_DEPS; d++)
        if (t[from].dep_idx[d] < 0) { t[from].dep_idx[d] = to; return; }
    assert(0);
}

int main(void) {
    reset(0);
    assert(services_check_cycles(t, 0) == 0);

    reset(4);
    assert(services_check_cycles(t, 4) == 0);

    reset(4); dep(0, 1); dep(1, 2); dep(2, 3);
    assert(services_check_cycles(t, 4) == 0);

    reset(4); dep(0, 1); dep(0, 2); dep(1, 3); dep(2, 3);
    assert(services_check_cycles(t, 4) == 0);

    reset(4); dep(3, 1); dep(3, 2); dep(1, 0); dep(2, 0);
    assert(services_check_cycles(t, 4) == 0);

    reset(1); dep(0, 0);
    assert(services_check_cycles(t, 1) == 1);

    reset(2); dep(0, 1); dep(1, 0);
    assert(services_check_cycles(t, 2) == 1);

    reset(3); dep(0, 1); dep(1, 2); dep(2, 0);
    assert(services_check_cycles(t, 3) == 1);

    reset(5); dep(0, 1); dep(2, 3); dep(3, 4); dep(4, 2);
    assert(services_check_cycles(t, 5) == 1);

    reset(4); dep(0, 1); dep(1, 0); dep(2, 3); dep(3, 2);
    assert(services_check_cycles(t, 4) == 2);

    reset(3); dep(0, 1); dep(1, 2); dep(2, 1);
    assert(services_check_cycles(t, 3) == 1);

    reset(2); dep(0, 1); dep(0, 7); dep(0, MAX_SERVICES + 5);
    assert(services_check_cycles(t, 2) == 0);

    reset(MAX_SERVICES);
    for (int i = 0; i + 1 < MAX_SERVICES; i++) dep(i, i + 1);
    assert(services_check_cycles(t, MAX_SERVICES) == 0);
    dep(MAX_SERVICES - 1, 0);
    assert(services_check_cycles(t, MAX_SERVICES) == 1);

    reset(MAX_SERVICES);
    for (int i = 1; i < MAX_SERVICES; i++)
        for (int d = 0; d < MAX_DEPS && d < i; d++) dep(i, i - 1 - d);
    assert(services_check_cycles(t, MAX_SERVICES) == 0);

    printf("test_service_cycles: ok\n");
    return 0;
}
