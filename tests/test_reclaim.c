#include "../service.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    long CAP = RECLAIM_CAP; /* 128 MiB */

    /* below the floor -> reclaim nothing */
    assert(reclaim_target(0, CAP) == 0);
    assert(reclaim_target(RECLAIM_FLOOR - 1, CAP) == 0);

    /* at/above floor, under cap -> half of current */
    assert(reclaim_target(RECLAIM_FLOOR, CAP) == RECLAIM_FLOOR / 2);
    assert(reclaim_target(100L * 1024 * 1024, CAP) == 50L * 1024 * 1024);

    /* half exceeds cap -> clamp to cap */
    assert(reclaim_target(400L * 1024 * 1024, CAP) == CAP);

    printf("all reclaim_target tests passed\n");
    return 0;
}
