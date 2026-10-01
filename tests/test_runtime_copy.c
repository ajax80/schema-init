#include "../reexec_state.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    static service_t src, dst;
    memset(&src, 0, sizeof src);
    memset(&dst, 0, sizeof dst);

    strcpy(src.name, "live");
    strcpy(src.exec, "/old/exec");
    src.flags = SVC_TIMER | SVC_NO_RESTART;
    src.child_pid = 4242;
    src.inst.state = 4; src.inst.prev_state = 3; src.inst.weight = 8; src.inst.flags = 0x55;
    src.restart_count = 3;
    src.dormant_count = 2;
    src.dormant_until.tv_sec = 900; src.dormant_until.tv_nsec = 7;
    src.last_start = 1759280000;
    src.start_time = 1759280001;
    src.stable_time.tv_sec = 33;
    src.failsafe_pid = 777;
    src.failsafe_start.tv_sec = 44;
    src.last_pet.tv_sec = 55;
    src.ready_path_verified = 1;
    src.notify_ready = 1;
    strcpy(src.notify_status, "Processing requests...");
    src.ctl_killed = 1;
    src.exit_status = 2;
    src.term_signal = 11;
    src.core_dumped = 1;
    src.has_exited = 1;
    src.timer_next.tv_sec = 1234; src.timer_next.tv_nsec = 999;
    src.spawn_time_mono.tv_sec = 66;
    strcpy(src.cgroup_path, "/sys/fs/cgroup/schema-init/live");
    src.is_frozen = 1;

    strcpy(dst.name, "fresh");
    strcpy(dst.exec, "/new/exec");
    dst.flags = SVC_ONESHOT;

    svc_runtime_copy(&dst, &src);

    assert(dst.child_pid == 4242);
    assert(dst.inst.state == 4 && dst.inst.prev_state == 3 && dst.inst.weight == 8 && dst.inst.flags == 0x55);
    assert(dst.restart_count == 3);
    assert(dst.dormant_count == 2);
    assert(dst.dormant_until.tv_sec == 900 && dst.dormant_until.tv_nsec == 7);
    assert(dst.last_start == 1759280000);
    assert(dst.start_time == 1759280001);
    assert(dst.stable_time.tv_sec == 33);
    assert(dst.failsafe_pid == 777);
    assert(dst.failsafe_start.tv_sec == 44);
    assert(dst.last_pet.tv_sec == 55);
    assert(dst.ready_path_verified == 1);
    assert(dst.notify_ready == 1);
    assert(strcmp(dst.notify_status, "Processing requests...") == 0);
    assert(dst.ctl_killed == 1);
    assert(dst.exit_status == 2);
    assert(dst.term_signal == 11);
    assert(dst.core_dumped == 1);
    assert(dst.has_exited == 1);
    assert(dst.timer_next.tv_sec == 1234 && dst.timer_next.tv_nsec == 999);
    assert(dst.spawn_time_mono.tv_sec == 66);
    assert(strcmp(dst.cgroup_path, "/sys/fs/cgroup/schema-init/live") == 0);
    assert(dst.is_frozen == 1);

    /* config half comes from the fresh parse, never from the live table */
    assert(strcmp(dst.name, "fresh") == 0);
    assert(strcmp(dst.exec, "/new/exec") == 0);
    assert(dst.flags == (SVC_ONESHOT | SVC_NO_RESTART));   /* only the stop hold carries */

    /* schema-ctl start cleared a configured restart=no: the live bit wins */
    {
        service_t f, l; memset(&f, 0, sizeof f); memset(&l, 0, sizeof l);
        f.flags = SVC_NO_RESTART;
        svc_runtime_copy(&f, &l);
        assert(!(f.flags & SVC_NO_RESTART));
    }

    /* run-once boot timer completed live: stays completed */
    {
        service_t f; memset(&f, 0, sizeof f);
        f.flags = SVC_TIMER; f.timer_interval_sec = 0;
        svc_carry_timer_done(&f, 1);
        assert(!(f.flags & SVC_TIMER));
    }
    /* not yet fired live: keeps its flag */
    {
        service_t f; memset(&f, 0, sizeof f);
        f.flags = SVC_TIMER; f.timer_interval_sec = 0;
        svc_carry_timer_done(&f, 0);
        assert(f.flags & SVC_TIMER);
    }
    /* interval and calendar timers never become terminal */
    {
        service_t f; memset(&f, 0, sizeof f);
        f.flags = SVC_TIMER; f.timer_interval_sec = 600;
        svc_carry_timer_done(&f, 1);
        assert(f.flags & SVC_TIMER);
        f.flags = SVC_TIMER | SVC_TIMER_CALENDAR; f.timer_interval_sec = 0;
        svc_carry_timer_done(&f, 1);
        assert(f.flags & SVC_TIMER);
    }

    printf("test_runtime_copy: OK\n");
    return 0;
}
