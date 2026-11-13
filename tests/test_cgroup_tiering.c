#include "../service.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    cgroup_tier_t p = cgroup_tiering(PRIO_PERIPHERAL, 0);
    assert(p.cpu_idle == 1);
    assert(p.cpu_weight == 0);   /* cpu.weight not written when cpu.idle=1 */
    assert(p.io_weight == 10);

    cgroup_tier_t s = cgroup_tiering(PRIO_STANDARD, 0);
    assert(s.cpu_idle == 0);
    assert(s.cpu_weight == 100);
    assert(s.io_weight == 100);

    cgroup_tier_t c = cgroup_tiering(PRIO_CRITICAL, 0);
    assert(c.cpu_idle == 0);
    assert(c.cpu_weight == 1000);
    assert(c.io_weight == 1000);

    /* an explicit cpu_weight= replaces the tier's CPU half, cpu.idle included;
     * io.weight stays with the priority class */
    cgroup_tier_t w = cgroup_tiering(PRIO_PERIPHERAL, 250);
    assert(w.cpu_idle == 0 && w.cpu_weight == 250 && w.io_weight == 10);
    w = cgroup_tiering(PRIO_CRITICAL, 1);
    assert(w.cpu_idle == 0 && w.cpu_weight == 1 && w.io_weight == 1000);

    /* cpu.max quota per 100000us period; >100% spans cores, junk writes nothing */
    assert(cpu_max_quota(0) == 0);
    assert(cpu_max_quota(-1) == 0);
    assert(cpu_max_quota(50) == 50000);
    assert(cpu_max_quota(100) == 100000);
    assert(cpu_max_quota(150) == 150000);
    assert(cpu_max_quota(10000) == 10000000);
    assert(cpu_max_quota(10001) == 0);

    /* survival posture: a STANDARD service is held to half a core or its own
     * tighter cap, and leaving restores its own cap (0 = write "max") */
    assert(survival_cpu_quota(0, 1) == 50000);
    assert(survival_cpu_quota(20, 1) == 20000);
    assert(survival_cpu_quota(150, 1) == 50000);
    assert(survival_cpu_quota(0, 0) == 0);
    assert(survival_cpu_quota(20, 0) == 20000);
    assert(survival_cpu_quota(150, 0) == 150000);

    /* no limit -> no soft reclaim buffer */
    assert(mem_high_bytes(0) == 0);
    assert(mem_high_bytes(-5) == 0);

    /* 90% of the hard cap, in bytes */
    assert(mem_high_bytes(100) == 94371840L);          /* 100*MiB*9/10 */
    assert(mem_high_bytes(1024) == 966367641L);        /* 1 GiB*9/10, integer trunc, no overflow */
    assert(mem_high_bytes(4096) == 3865470566LL);      /* 4 GiB*9/10: > 32-bit LONG_MAX */

    printf("all cgroup_tiering tests passed\n");
    /* memory.low: MB to bytes, nothing written for 0 */
    assert(mem_low_bytes(0) == 0);
    assert(mem_low_bytes(-1) == 0);
    assert(mem_low_bytes(256) == 256LL * 1048576);

    /* memory.swap.max: CRITICAL stays out of swap unless swap_max= says so;
     * the others are left alone; "max" and 0 are both explicit */
    assert(swap_max_value(PRIO_CRITICAL, 0, 0) == 0);
    assert(swap_max_value(PRIO_STANDARD, 0, 0) == CG_SWAP_SKIP);
    assert(swap_max_value(PRIO_PERIPHERAL, 0, 0) == CG_SWAP_SKIP);
    assert(swap_max_value(PRIO_CRITICAL, 1, -1) == CG_SWAP_MAX);
    assert(swap_max_value(PRIO_CRITICAL, 1, 128) == 128LL * 1048576);
    assert(swap_max_value(PRIO_STANDARD, 1, 0) == 0);

    return 0;
}
