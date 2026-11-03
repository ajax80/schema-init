#include "../engine.h"
#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void test_pid1_defaults(void) {
    assert(!strcmp(engine_paths.ctl_fallback, "./run/schema-init.sock"));
    assert(!strcmp(engine_paths.env_fallback, "./run/schema-init/env"));
    assert(!strcmp(engine_paths.svc_fallback, "./services"));
    assert(!strcmp(engine_paths.tag, "schema-init"));
    assert(service_log_dir == NULL);
    assert(engine_reload_integrity == 1);
    assert(sizeof(((service_t *)0)->cgroup_path) == 256);
}

static void test_mkdir_p(void) {
    char base[] = "/tmp/schema-mkdirp-XXXXXX";
    char leaf[128];
    struct stat st;
    assert(mkdtemp(base));
    snprintf(leaf, sizeof leaf, "%s/a/b/c", base);
    assert(engine_mkdir_p(leaf, 0700) == 0);
    assert(stat(leaf, &st) == 0 && S_ISDIR(st.st_mode));
    assert(engine_mkdir_p(leaf, 0700) == 0);
}

/* crash-loop breaker: four parks with backoff, the fifth verdict excises and
 * marks circuit_broken; critical / no_excise services park forever. */
static void test_circuit_breaker(void) {
    service_t s;
    int k;
    memset(&s, 0, sizeof s);
    strcpy(s.name, "loopy");
    for (k = 1; k <= 4; k++) {
        s.inst.state = STATE_EXCISED;
        engine_friction_verdict(&s);
        assert(s.inst.state == STATE_DORMANT && s.dormant_count == k && !s.circuit_broken);
    }
    s.inst.state = STATE_EXCISED;
    engine_friction_verdict(&s);
    assert(s.inst.state == STATE_EXCISED && s.circuit_broken == 1);

    memset(&s, 0, sizeof s);
    strcpy(s.name, "crit");
    s.flags = SVC_CRITICAL;
    for (k = 0; k < 8; k++) {
        s.inst.state = STATE_EXCISED;
        engine_friction_verdict(&s);
        assert(s.inst.state == STATE_DORMANT && !s.circuit_broken);
    }
    memset(&s, 0, sizeof s);
    strcpy(s.name, "keep");
    s.no_excise = 1;
    for (k = 0; k < 8; k++) {
        s.inst.state = STATE_EXCISED;
        engine_friction_verdict(&s);
        assert(s.inst.state == STATE_DORMANT && !s.circuit_broken);
    }
}

int main(void) {
    test_pid1_defaults();
    test_circuit_breaker();
    test_mkdir_p();
    assert(!strcmp(engine_paths.ctl_sock, "/run/schema-init.sock"));
    assert(!strcmp(engine_paths.run_dir, "/run/schema-init"));
    assert(!strcmp(engine_paths.notify_sock, "/run/schema-init/notify"));
    assert(!strcmp(engine_paths.shm_name, "/schema-init"));
    assert(!strcmp(engine_paths.timer_stamp_dir, "/var/lib/schema-init/timers"));
    assert(!strcmp(engine_paths.svc_dir, "/etc/schema-init/services"));
    assert(!strcmp(service_cgroup_root, "/sys/fs/cgroup/schema-init"));
    assert(!strcmp(service_notify_sock, "/run/schema-init/notify"));

    service_t s;
    memset(&s, 0, sizeof s);
    strcpy(s.name, "foo");
    assert(!strcmp(service_cgroup_path_for(&s), "/sys/fs/cgroup/schema-init/foo"));
    service_cgroup_root = "/tmp/cg-test";
    assert(!strcmp(service_cgroup_path_for(&s), "/tmp/cg-test/foo"));

    char line[] = "reboot";
    assert(engine_hooks.loop_pressure && engine_hooks.loop_tail && engine_hooks.ctl_extra
           && engine_hooks.shut_log && engine_hooks.on_sigcont && engine_hooks.on_reaped);
    engine_hooks.loop_pressure(1);
    int tick = 7;
    assert(engine_hooks.wake_due && engine_hooks.wake_due(1000, &tick) == INT64_MAX && tick == 7);
    engine_hooks.loop_tail();
    engine_hooks.on_sigcont();
    engine_hooks.shut_log("test");
    assert(engine_hooks.ctl_extra(-1, line) == 0);
    assert(engine_hooks.on_reaped(12345, 0) == 0);

    printf("test_engine_conf: ok\n");
    return 0;
}
