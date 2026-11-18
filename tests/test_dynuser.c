#include "../dynuser.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uid_t taken_set[8];
static int taken_n;
static int all_taken;

static int taken(uid_t u, void *ctx) {
    (void)ctx;
    if (all_taken) return 1;
    for (int i = 0; i < taken_n; i++)
        if (taken_set[i] == u) return 1;
    return 0;
}

int main(void) {
    /* the start is 61184 + FNV-1a(name) % 4336 (computed independently) */
    assert(dynuser_pick("earlyoom", taken, NULL) == 64605);
    assert(dynuser_pick("fwupd-refresh", taken, NULL) == 62743);
    /* a taken start probes upward */
    taken_set[taken_n++] = 64605;
    taken_set[taken_n++] = 64606;
    assert(dynuser_pick("earlyoom", taken, NULL) == 64607);
    /* the top of the range wraps to the bottom */
    taken_n = 0;
    for (uid_t u = 65519; taken_n < 1; u++) taken_set[taken_n++] = u;
    assert(DYNUSER_MIN == 61184 && DYNUSER_MAX == 65519);
    {
        /* a name whose start is the last uid: find one by search */
        char name[32];
        int found = 0;
        for (int i = 0; i < 100000 && !found; i++) {
            snprintf(name, sizeof name, "w%d", i);
            taken_n = 0;
            if (dynuser_pick(name, taken, NULL) == DYNUSER_MAX) found = 1;
        }
        assert(found);
        taken_set[taken_n++] = DYNUSER_MAX;
        assert(dynuser_pick(name, taken, NULL) == DYNUSER_MIN);
    }
    /* nothing free: 0, the start is refused */
    all_taken = 1;
    assert(dynuser_pick("earlyoom", taken, NULL) == 0);
    printf("all dynuser tests passed\n");
    return 0;
}
