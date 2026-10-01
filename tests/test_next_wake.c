#include "../service.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    struct timespec ts = { 10, 1 };
    assert(ts_ms_ceil(&ts) == 10001);
    ts.tv_nsec = 0;
    assert(ts_ms_ceil(&ts) == 10000);
    ts.tv_nsec = 999999999;
    assert(ts_ms_ceil(&ts) == 11000);

    assert(wake_min(WAKE_NONE, 5000, 1000, WAKE_NONE) == 4000);
    assert(wake_min(3000, 5000, 1000, WAKE_NONE) == 3000);
    assert(wake_min(WAKE_NONE, 1000, 1000, WAKE_NONE) == 0);
    assert(wake_min(WAKE_NONE, 500, 1000, WAKE_NONE) == 0);
    assert(wake_min(WAKE_NONE, INT64_MIN / 2, 1000, WAKE_NONE) == 0);
    assert(wake_min(WAKE_NONE, 1000 + 3600000, 1000, WAKE_CAL_CAP_MS) == WAKE_CAL_CAP_MS);
    assert(wake_min(WAKE_NONE, 1000 + 30000, 1000, WAKE_CAL_CAP_MS) == 30000);

    assert(wake_timeout(WAKE_NONE, 0, 250) == -1);
    assert(wake_timeout(WAKE_NONE, 1, 250) == 250);
    assert(wake_timeout(4000, 1, 250) == 250);
    assert(wake_timeout(100, 1, 250) == 100);
    assert(wake_timeout(4000, 0, 250) == 4000);
    assert(wake_timeout(0, 0, 250) == 0);
    assert(wake_timeout((int64_t)INT_MAX + 5, 0, 250) == INT_MAX);

    assert(wd_pet_ms(60) == 20000);
    assert(wd_pet_ms(30) == 10000);
    assert(wd_pet_ms(2) == 666);
    assert(wd_pet_ms(1) == 333);
    assert(wd_pet_ms(0) == 5000);
    assert(wd_pet_ms(-1) == 5000);

    printf("all next_wake tests passed\n");
    return 0;
}
