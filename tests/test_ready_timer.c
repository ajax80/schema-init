#include "../service.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* plain daemon: the stable timer as always */
    assert(!ready_timer_due(9, STABLE_SECS, 0));
    assert(ready_timer_due(10, STABLE_SECS, 0));
    assert(ready_timer_due(0, 0, 0));

    /* ready_bus_name, no reporter has spoken: no one will report the name, keep the timer */
    assert(ready_timer_due(10, STABLE_SECS, 0));

    /* ready_bus_name with a live reporter: TuneD claims at ~18 s, the timer must not beat it */
    assert(!ready_timer_due(10, STABLE_SECS, 1));
    assert(!ready_timer_due(18, STABLE_SECS, 1));
    assert(!ready_timer_due(BUS_NAME_WAIT_SECS - 1, STABLE_SECS, 1));
    assert(ready_timer_due(BUS_NAME_WAIT_SECS, STABLE_SECS, 1));
    /* a stable_secs past the ceiling still holds */
    assert(!ready_timer_due(BUS_NAME_WAIT_SECS, BUS_NAME_WAIT_SECS + 30, 1));
    assert(ready_timer_due(BUS_NAME_WAIT_SECS + 30, BUS_NAME_WAIT_SECS + 30, 1));

    printf("test_ready_timer: ok\n");
    return 0;
}
