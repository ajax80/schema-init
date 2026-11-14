#include "../engine.h"
#include "../service.h"
#include <signal.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

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
    assert(s.inst.state == STATE_EXCISED && s.circuit_broken == 1 && s.excise_reason == EXC_BREAKER);
    s.inst.state = STATE_EXCISED;   /* a breaker that has tripped trips again on the next verdict */
    engine_friction_verdict(&s);
    assert(s.circuit_broken == 1);

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

/* an unrequested death names its cause; a stop or a clean exit 0 is none */
static void test_excise_reason_from_exit(void) {
    service_t s;
    memset(&s, 0, sizeof s);
    s.has_exited = 1; s.exit_status = 3;
    assert(excise_reason_from_exit(&s) == EXC_EXIT_CODE);
    s.exit_status = 0;
    assert(excise_reason_from_exit(&s) == EXC_NONE);
    s.exit_status = -1; s.term_signal = SIGTERM;
    assert(excise_reason_from_exit(&s) == EXC_SIGNAL);
    s.core_dumped = 1;
    assert(excise_reason_from_exit(&s) == EXC_CORE_DUMP);
    s.ctl_killed = 1;
    assert(excise_reason_from_exit(&s) == EXC_NONE);
    assert(strcmp(excise_reason_name(EXC_START_TIMEOUT), "start-timeout") == 0);
    assert(strcmp(excise_reason_name(EXC_FUSE), "fuse") == 0);
    assert(strcmp(excise_reason_name(EXC_PROBE), "probe-failed") == 0);
    assert(strcmp(excise_reason_name(200), "unknown") == 0);
}

/* audit 2026-11-12, 2.5: the hardware watchdog is opt-in */
static void test_watchdog_opt_in(void) {
    service_t sv[2];
    memset(sv, 0, sizeof sv);
    assert(watchdog_arm_reason("quiet splash", 0, sv, 2) == NULL);
    assert(watchdog_arm_reason(NULL, 0, sv, 2) == NULL);
    assert(watchdog_arm_reason("quiet schema.watchdog=1", 0, sv, 2) != NULL);
    assert(watchdog_arm_reason("xschema.watchdog=1", 0, sv, 2) == NULL);
    assert(watchdog_arm_reason("schema.watchdog=10", 0, sv, 2) == NULL);
    assert(watchdog_arm_reason("", 1, sv, 2) != NULL);
    sv[1].watchdog_timeout_ms = 500;
    assert(strstr(watchdog_arm_reason("", 0, sv, 2), "watchdog_timeout_ms"));
    /* =0 on the command line overrides the file and the services */
    assert(watchdog_arm_reason("schema.watchdog=0", 1, sv, 2) == NULL);
    /* last one wins */
    assert(watchdog_arm_reason("schema.watchdog=1 schema.watchdog=0", 0, sv, 2) == NULL);
    assert(watchdog_arm_reason("schema.watchdog=0 schema.watchdog=1", 0, sv, 2) != NULL);
}

/* audit 3.4: what still counts as alive after the SIGKILL broadcast */
static void test_proc_stat_is_user(void) {
    assert(proc_stat_is_user("812 (bash) S 1 812 812 0 -1 4194560 100"));
    assert(!proc_stat_is_user("37 (kworker/u16:2) I 2 0 0 0 -1 69238880 0"));   /* PF_KTHREAD */
    /* a usermode helper: kthreadd's child, but not a kernel thread */
    assert(proc_stat_is_user("4021 (schema-coredump) R 2 0 0 0 -1 4194560 12"));
    assert(proc_stat_is_user("900 (a) b) 2 c) Z 1 900 900 0 -1 4194560 0"));   /* comm with ") 2 " in it */
    assert(!proc_stat_is_user("garbage"));
}

static void touch(const char *dir, const char *name) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    assert(f);
    fclose(f);
}

/* /usr/lib/modules-load.d is read too, with systemd's override and mask rules */
static void test_modload_conf_list(void) {
    char base[] = "/tmp/schema-modload-XXXXXX", etc[64], lib[64], alias[64], p[128], gone[64], cmd[96];
    const char *out[8];
    int dropped;
    assert(mkdtemp(base));
    snprintf(etc, sizeof etc, "%s/etc", base);
    snprintf(lib, sizeof lib, "%s/lib", base);
    snprintf(alias, sizeof alias, "%s/alias", base);
    snprintf(gone, sizeof gone, "%s/missing", base);
    assert(mkdir(etc, 0755) == 0 && mkdir(lib, 0755) == 0 && symlink(lib, alias) == 0);
    touch(etc, "x.conf");
    touch(lib, "x.conf");
    touch(lib, "a.conf");
    touch(lib, "z.conf");
    touch(lib, "m.conf");
    touch(lib, "y.conf");
    touch(lib, "notes.txt");
    snprintf(p, sizeof p, "%s/m.conf", etc);
    assert(symlink("/dev/null", p) == 0);
    snprintf(p, sizeof p, "%s/y.conf", etc);     /* a directory is not a drop-in */
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof p, "%s/dangling.conf", etc);
    assert(symlink("/nonexistent", p) == 0);
    /* alias is lib under another name (usrmerge's /lib): read once */
    const char *dirs[] = { etc, gone, lib, alias, NULL };
    int n = modload_conf_list(dirs, out, 8, &dropped);
    const char *want[] = { "lib/a.conf", "etc/x.conf", "lib/y.conf", "lib/z.conf" };
    assert(n == 4 && dropped == 0);
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", base, want[i]);
        assert(!strcmp(out[i], p));
    }
    assert(modload_conf_list(dirs, out, 2, &dropped) == 2 && dropped == 2);
    const char *none[] = { gone, NULL };
    assert(modload_conf_list(none, out, 8, &dropped) == 0 && dropped == 0);
    snprintf(cmd, sizeof cmd, "rm -rf %s", base);
    assert(system(cmd) == 0);
}

static void test_cmdline_modules_load(void) {
    char names[4][256];
    assert(cmdline_modules_load("ro modules_load=vfio-pci,kvm quiet modules-load=tun", names, 4) == 3);
    assert(!strcmp(names[0], "vfio-pci") && !strcmp(names[1], "kvm") && !strcmp(names[2], "tun"));
    assert(cmdline_modules_load("rd.modules_load=x xmodules_load=y modules_load= modules_load=,a,,", names, 4) == 1);
    assert(!strcmp(names[0], "a"));
    assert(cmdline_modules_load("modules_load=a,b,c,d,e", names, 4) == 4);
    assert(cmdline_modules_load("", names, 4) == 0);
}

int main(void) {
    test_modload_conf_list();
    test_cmdline_modules_load();
    test_watchdog_opt_in();
    test_proc_stat_is_user();
    test_pid1_defaults();
    test_excise_reason_from_exit();
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
