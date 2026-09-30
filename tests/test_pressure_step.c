#include "../service.h"
#include <assert.h>
#include <stdio.h>

static uint64_t run_until_exit(uint64_t spacing) {
    int under = 0;
    uint64_t last = 0, t = 1000;
    assert(pressure_step(&under, t, &last, 1) == PRESSURE_ENTER);
    uint64_t stall_end = t;
    for (;;) {
        t += spacing;
        if (pressure_step(&under, t, &last, 0) == PRESSURE_EXIT)
            return t - stall_end;
        assert(t - stall_end < 60000);
    }
}

int main(void) {
    int under = 0;
    uint64_t last = 0;

    assert(pressure_step(&under, 1000, &last, 0) == PRESSURE_NONE && !under);

    assert(pressure_step(&under, 1000, &last, 1) == PRESSURE_ENTER && under);
    assert(last == 1000);
    assert(pressure_step(&under, 1250, &last, 1) == PRESSURE_NONE && under);
    assert(last == 1250);

    assert(pressure_step(&under, 1250 + THAW_DELAY_MS - 1, &last, 0) == PRESSURE_NONE && under);
    assert(pressure_step(&under, 1250 + THAW_DELAY_MS - 1, &last, 1) == PRESSURE_NONE && under);
    uint64_t rearmed = 1250 + THAW_DELAY_MS - 1;
    assert(pressure_step(&under, rearmed + THAW_DELAY_MS - 1, &last, 0) == PRESSURE_NONE && under);
    assert(pressure_step(&under, rearmed + THAW_DELAY_MS, &last, 0) == PRESSURE_EXIT && !under);
    assert(pressure_step(&under, rearmed + THAW_DELAY_MS + 250, &last, 0) == PRESSURE_NONE && !under);

    assert(run_until_exit(250) == THAW_DELAY_MS);
    assert(run_until_exit(5000) == THAW_DELAY_MS);

    printf("all pressure_step tests passed\n");
    return 0;
}
