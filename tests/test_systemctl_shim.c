#include "../systemctl_shim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

static void test_strip_suffix(void) {
    char b[256];
    assert(strcmp(strip_service_suffix("foo.service", b, sizeof b), "foo") == 0);
    assert(strcmp(strip_service_suffix("foo", b, sizeof b), "foo") == 0);
    assert(strcmp(strip_service_suffix("foo.socket", b, sizeof b), "foo") == 0);
    assert(strcmp(unit_queue_name("foo.socket", b, sizeof b), "foo.socket") == 0);
    assert(strcmp(strip_service_suffix("foo.timer", b, sizeof b), "foo") == 0);
    assert(strcmp(unit_queue_name("foo.timer", b, sizeof b), "foo.timer") == 0);
    assert(strcmp(unit_queue_name("foo", b, sizeof b), "foo.service") == 0);
}

static void test_supported(void) {
    assert(unit_supported("foo.service") == 1);
    assert(unit_supported("foo") == 1);
    assert(unit_supported("foo@bar.service") == 0);
    assert(unit_supported("getty@tty1.service") == 0);
    assert(unit_supported("foo.socket") == 1);
    assert(unit_supported("foo@.socket") == 0);
    assert(unit_supported("foo.timer") == 1);
    assert(unit_supported("foo@.timer") == 0);
    assert(unit_supported("foo.path") == 0);
    assert(unit_supported("foo.target") == 0);
    assert(unit_supported("foo.mount") == 0);
    assert(unit_supported("foo.slice") == 0);
    assert(unit_supported("foo.scope") == 0);
}

static char sandbox[256];

static void setup_sandbox(void) {
    char tmpl[] = "/tmp/shimtestXXXXXX";
    char *d = mkdtemp(tmpl);
    assert(d);
    snprintf(sandbox, sizeof sandbox, "%s", d);
    char state[300], svc[300], unitdir[300];
    snprintf(state, sizeof state, "%s/state", sandbox);
    snprintf(svc, sizeof svc, "%s/svc", sandbox);
    snprintf(unitdir, sizeof unitdir, "%s/units", sandbox);
    mkdir(state, 0755); mkdir(svc, 0755); mkdir(unitdir, 0755);
    setenv("SCHEMA_STATE_DIR", state, 1);
    setenv("SCHEMA_SVC_DIR", svc, 1);
    setenv("SCHEMA_UNIT_DIR", unitdir, 1);
    char noshm[300];
    snprintf(noshm, sizeof noshm, "%s/no-shm", sandbox);
    setenv("SCHEMA_SHM_PATH", noshm, 1);
}

static int run(const char *a, const char *b) {
    char *argv[4]; int n = 0;
    argv[n++] = (char *)"systemctl";
    argv[n++] = (char *)a;
    if (b) argv[n++] = (char *)b;
    argv[n] = NULL;
    return shim_dispatch(n, argv);
}

static int run3(const char *a, const char *b, const char *c) {
    char *argv[5]; int n = 0;
    argv[n++] = (char *)"systemctl";
    argv[n++] = (char *)a;
    if (b) argv[n++] = (char *)b;
    if (c) argv[n++] = (char *)c;
    argv[n] = NULL;
    return shim_dispatch(n, argv);
}

static void write_unit(const char *name) {
    char p[400], *d = getenv("SCHEMA_UNIT_DIR");
    snprintf(p, sizeof p, "%s/%s", d, name);
    FILE *f = fopen(p, "w"); assert(f); fputs("[Service]\n", f); fclose(f);
}

static void test_enable_queue(void) {
    setup_sandbox();
    write_unit("foo.service");
    assert(run("enable", "foo.service") == 0);
    char qp[400];
    snprintf(qp, sizeof qp, "%s/state/pending.list", sandbox);
    FILE *f = fopen(qp, "r"); assert(f);
    char line[400]; int count = 0;
    while (fgets(line, sizeof line, f)) count++;
    fclose(f);
    assert(count == 1);
    /* dedup */
    assert(run("enable", "foo.service") == 0);
    f = fopen(qp, "r"); count = 0;
    while (fgets(line, sizeof line, f)) count++;
    fclose(f);
    assert(count == 1);
    /* is-enabled true */
    assert(run("is-enabled", "foo") == 0);
    /* disable removes */
    assert(run("disable", "foo") == 0);
    assert(run("is-enabled", "foo") == 1);
    /* skip unsupported, still exit 0, not queued */
    assert(run("enable", "foo@bar.service") == 0);
    assert(run("is-enabled", "foo@bar.service") == 1);
    /* preset behaves like enable */
    assert(run("preset", "foo.service") == 0);
    assert(run("is-enabled", "foo") == 0);
    /* a timer is queued under its own name, and disable takes it back */
    assert(run("enable", "bar.timer") == 0);
    assert(run("is-enabled", "bar.timer") == 0);
    assert(run("disable", "bar.timer") == 0);
    assert(run("is-enabled", "bar.timer") == 1);
    /* so is a socket */
    assert(run("enable", "baz.socket") == 0);
    assert(run("is-enabled", "baz.socket") == 0);
    assert(run("disable", "baz.socket") == 0);
    assert(run("is-enabled", "baz.socket") == 1);
}

static void make_ctl_stub(const char *active_name) {
    char p[400];
    snprintf(p, sizeof p, "%s/schema-ctl", sandbox);
    FILE *f = fopen(p, "w"); assert(f);
    fprintf(f,
        "#!/bin/sh\n"
        "echo \"$@\" >> \"%s/ctl.log\"\n"
        "if [ \"$1\" = status ]; then\n"
        "  echo 'service.%s.state=FULL_TRUST'\n"
        "  echo 'service.sleeper.state=DORMANT'\n"
        "fi\n"
        "exit 0\n", sandbox, active_name);
    fchmod(fileno(f), 0755);
    fclose(f);
    char ctl[400];
    snprintf(ctl, sizeof ctl, "%s/schema-ctl", sandbox);
    setenv("SCHEMA_CTL", ctl, 1);
}

static int ctl_log_has(const char *needle) {
    char p[400]; snprintf(p, sizeof p, "%s/ctl.log", sandbox);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char line[400]; int hit = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) { hit = 1; break; }
    fclose(f); return hit;
}

static void test_lifecycle(void) {
    setup_sandbox();
    make_ctl_stub("running");
    char svc[512];
    snprintf(svc, sizeof svc, "%s/svc/running.svc", sandbox);
    FILE *f = fopen(svc, "w"); assert(f); fputs("name=running\n", f); fclose(f);
    assert(run("start", "running.service") == 0);
    assert(ctl_log_has("start running"));
    assert(run("start", "ghost.service") == 0);
    assert(!ctl_log_has("start ghost"));
    assert(run("daemon-reload", NULL) == 0);
    assert(ctl_log_has("reload"));
    assert(run("is-active", "running") == 0);
    assert(run("is-active", "sleeper") == 3);
    assert(run("is-active", "nope") == 3);
    /* a timer: start/restart would fire the job, stop passes through */
    snprintf(svc, sizeof svc, "%s/svc/tick.svc", sandbox);
    f = fopen(svc, "w"); assert(f); fputs("name=tick\non_calendar=00:00\n", f); fclose(f);
    assert(run("start", "tick.timer") == 0);
    assert(run("restart", "tick.timer") == 0);
    assert(!ctl_log_has("tick"));
    assert(run("stop", "tick.timer") == 0);
    assert(ctl_log_has("stop tick"));
    /* a socket is held whenever its .svc is loaded: start is a no-op, stop stops the service */
    snprintf(svc, sizeof svc, "%s/svc/sock.svc", sandbox);
    f = fopen(svc, "w"); assert(f); fputs("name=sock\nlisten=stream:/run/sock\n", f); fclose(f);
    assert(run("start", "sock.socket") == 0);
    assert(run("restart", "sock.socket") == 0);
    assert(!ctl_log_has("sock"));
    assert(run("stop", "sock.socket") == 0);
    assert(ctl_log_has("stop sock"));
}

static void test_flags_and_safety(void) {
    setup_sandbox();
    write_unit("foo.service");
    assert(run3("enable", "--now", "foo.service") == 0);
    assert(run("is-enabled", "foo") == 0);
    assert(run("frobnicate", "foo") == 0);
    char *argv[1] = { (char *)"systemctl" };
    assert(shim_dispatch(1, argv) == 0);
    setup_sandbox();
    write_unit("bar.service");
    assert(run3("--user", "enable", "bar.service") == 0);
    assert(run("is-enabled", "bar") == 1);
    make_ctl_stub("baz");
    char svc[512];
    snprintf(svc, sizeof svc, "%s/svc/baz.svc", sandbox);
    FILE *f = fopen(svc, "w"); assert(f); fputs("name=baz\n", f); fclose(f);
    write_unit("baz.service");
    assert(run3("enable", "--now", "baz.service") == 0);
    assert(ctl_log_has("start baz"));
}

static void test_flag_before_verb(void) {
    setup_sandbox();
    write_unit("foo.service");
    assert(run3("--no-reload", "preset", "foo.service") == 0);
    char qp[400];
    snprintf(qp, sizeof qp, "%s/state/pending.list", sandbox);
    FILE *f = fopen(qp, "r"); assert(f);
    char line[400]; int has_foo = 0, has_preset = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strstr(line, "foo.service")) has_foo = 1;
        if (strcmp(line, "preset") == 0) has_preset = 1;
    }
    fclose(f);
    assert(has_foo);
    assert(!has_preset);
    assert(run3("--quiet", "is-enabled", "foo") == 0);
}

static int run_as(const char *argv0, const char *a, const char *b) {
    char *argv[4]; int n = 0;
    argv[n++] = (char *)argv0;
    if (a) argv[n++] = (char *)a;
    if (b) argv[n++] = (char *)b;
    argv[n] = NULL;
    return shim_dispatch(n, argv);
}

static void ctl_log_clear(void) {
    char p[400]; snprintf(p, sizeof p, "%s/ctl.log", sandbox); unlink(p);
}

static void test_power(void) {
    setup_sandbox();
    make_ctl_stub("x");
    ctl_log_clear(); assert(run_as("systemctl", "reboot", NULL) == 0); assert(ctl_log_has("reboot"));
    ctl_log_clear(); assert(run_as("systemctl", "poweroff", NULL) == 0); assert(ctl_log_has("poweroff"));
    ctl_log_clear(); assert(run_as("systemctl", "halt", NULL) == 0); assert(ctl_log_has("poweroff"));
    ctl_log_clear(); assert(run_as("/usr/sbin/reboot", NULL, NULL) == 0); assert(ctl_log_has("reboot"));
    ctl_log_clear(); assert(run_as("poweroff", NULL, NULL) == 0); assert(ctl_log_has("poweroff"));
    ctl_log_clear(); assert(run_as("shutdown", "-r", "now") == 0); assert(ctl_log_has("reboot"));
    ctl_log_clear(); assert(run_as("shutdown", "now", NULL) == 0); assert(ctl_log_has("poweroff"));
    ctl_log_clear(); assert(run_as("shutdown", "-c", NULL) == 0); assert(!ctl_log_has("poweroff") && !ctl_log_has("reboot"));
    ctl_log_clear(); assert(run_as("systemctl", "--no-wall", "reboot") == 0); assert(ctl_log_has("reboot"));
    ctl_log_clear(); run_as("systemctl", "status", "reboot"); assert(!ctl_log_has("reboot"));   /* a unit arg, not a verb */
}

static void test_unit_aliases(void) {
    setup_sandbox();
    make_ctl_stub("network-manager");
    char svc[512];
    snprintf(svc, sizeof svc, "%s/svc/network-manager.svc", sandbox);
    FILE *f = fopen(svc, "w"); assert(f); fputs("name=network-manager\n", f); fclose(f);
    assert(run("restart", "NetworkManager.service") == 0);
    assert(ctl_log_has("restart network-manager"));
    assert(run("is-active", "NetworkManager.service") == 0);
    assert(run("is-enabled", "NetworkManager.service") == 0);
    snprintf(svc, sizeof svc, "%s/svc/polkit.svc", sandbox);
    f = fopen(svc, "w"); assert(f); fputs("name=polkit\n", f); fclose(f);
    snprintf(svc, sizeof svc, "%s/svc/polkitd.svc", sandbox);
    f = fopen(svc, "w"); assert(f); fputs("name=polkitd\n", f); fclose(f);
    ctl_log_clear();
    assert(run("start", "polkit.service") == 0);
    assert(ctl_log_has("start polkit") && !ctl_log_has("start polkitd"));
    snprintf(svc, sizeof svc, "%s/svc/bluetoothd.svc", sandbox);
    f = fopen(svc, "w"); assert(f); fputs("name=bluetoothd\n", f); fclose(f);
    write_unit("bluetooth.service");
    assert(run("enable", "bluetooth.service") == 0);
    assert(run("disable", "bluetooth.service") == 0);
    assert(access(svc, F_OK) != 0);
}

static void test_is_active_from_shm(void) {
    setup_sandbox();
    char p[400];
    snprintf(p, sizeof p, "%s/schema-ctl", sandbox);
    FILE *f = fopen(p, "w"); assert(f);
    fprintf(f, "#!/bin/sh\necho \"$@\" >> \"%s/ctl.log\"\nexit 1\n", sandbox);
    fchmod(fileno(f), 0755); fclose(f);
    setenv("SCHEMA_CTL", p, 1);
    static schema_shm_t t;
    memset(&t, 0, sizeof t);
    t.seq = 7; t.count = 3;
    snprintf(t.svc[0].name, sizeof t.svc[0].name, "network-manager"); t.svc[0].state = STATE_FULL_TRUST;
    snprintf(t.svc[1].name, sizeof t.svc[1].name, "sleeper");         t.svc[1].state = STATE_DORMANT;
    snprintf(t.svc[2].name, sizeof t.svc[2].name, "gone");            t.svc[2].state = STATE_EXCISED;
    snprintf(p, sizeof p, "%s/shm", sandbox);
    f = fopen(p, "w"); assert(f); assert(fwrite(&t, sizeof t, 1, f) == 1); fclose(f);
    setenv("SCHEMA_SHM_PATH", p, 1);
    assert(run("is-active", "NetworkManager.service") == 0);
    assert(run("is-active", "sleeper") == 3);
    assert(run("is-active", "gone") == 3);
    assert(run("is-active", "nope") == 3);
    assert(!ctl_log_has("status"));
}

static void test_passthrough(void) {
    char dir[] = "/tmp/shim-pt-XXXXXX";
    assert(mkdtemp(dir));
    char comm[256], real[256];
    snprintf(comm, sizeof comm, "%s/comm", dir);
    snprintf(real, sizeof real, "%s/systemctl.real", dir);
    setenv("SCHEMA_PID1_COMM", comm, 1);
    setenv("SCHEMA_REAL_SYSTEMCTL", real, 1);
    FILE *f;

    f = fopen(comm, "w"); fputs("systemd\n", f); fclose(f);
    assert(shim_passthrough() == NULL);                 /* real missing -> handle it */
    f = fopen(real, "w"); fputs("#!/bin/sh\n", f); fclose(f);
    chmod(real, 0755);
    assert(shim_passthrough() != NULL);                 /* PID 1 systemd -> real */
    assert(strcmp(shim_passthrough(), real) == 0);

    f = fopen(comm, "w"); fputs("schema-init\n", f); fclose(f);
    assert(shim_passthrough() == NULL);                 /* schema-init PID 1 -> shim */

    unlink(comm);
    assert(shim_passthrough() == NULL);                 /* unreadable -> shim */

    unlink(real); rmdir(dir);
    unsetenv("SCHEMA_PID1_COMM");
    unsetenv("SCHEMA_REAL_SYSTEMCTL");
}

int main(void) {
    test_strip_suffix();
    test_supported();
    printf("task1 systemctl-shim tests passed\n");
    test_enable_queue();
    printf("task2 systemctl-shim tests passed\n");
    test_lifecycle();
    printf("task3 systemctl-shim tests passed\n");
    test_flags_and_safety();
    printf("task4 systemctl-shim tests passed\n");
    test_flag_before_verb();
    printf("task5 systemctl-shim tests passed\n");
    test_power();
    printf("power systemctl-shim tests passed\n");
    test_passthrough();
    test_unit_aliases();
    test_is_active_from_shm();
    printf("passthrough systemctl-shim tests passed\n");
    return 0;
}
