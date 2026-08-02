#include "../service.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    cgroup_tier_t p = cgroup_tiering(PRIO_PERIPHERAL);
    assert(p.cpu_idle == 1);
    assert(p.cpu_weight == 0);   /* cpu.weight not written when cpu.idle=1 */
    assert(p.io_weight == 10);

    cgroup_tier_t s = cgroup_tiering(PRIO_STANDARD);
    assert(s.cpu_idle == 0);
    assert(s.cpu_weight == 100);
    assert(s.io_weight == 100);

    cgroup_tier_t c = cgroup_tiering(PRIO_CRITICAL);
    assert(c.cpu_idle == 0);
    assert(c.cpu_weight == 1000);
    assert(c.io_weight == 1000);

    /* no limit -> no soft reclaim buffer */
    assert(mem_high_bytes(0) == 0);
    assert(mem_high_bytes(-5) == 0);

    /* 90% of the hard cap, in bytes */
    assert(mem_high_bytes(100) == 94371840L);          /* 100*MiB*9/10 */
    assert(mem_high_bytes(1024) == 966367641L);        /* 1 GiB*9/10, integer trunc, no overflow */

    printf("all cgroup_tiering tests passed\n");
    return 0;
}
