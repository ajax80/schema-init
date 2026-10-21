#include "../engine.h"
#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
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
           && engine_hooks.shut_log && engine_hooks.on_sigcont);
    engine_hooks.loop_pressure();
    engine_hooks.loop_tail();
    engine_hooks.on_sigcont();
    engine_hooks.shut_log("test");
    assert(engine_hooks.ctl_extra(-1, line) == 0);

    printf("test_engine_conf: ok\n");
    return 0;
}
