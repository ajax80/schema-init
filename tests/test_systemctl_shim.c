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
    assert(strcmp(strip_service_suffix("foo.socket", b, sizeof b), "foo.socket") == 0);
}

static void test_supported(void) {
    assert(unit_supported("foo.service") == 1);
    assert(unit_supported("foo") == 1);
    assert(unit_supported("foo@bar.service") == 0);
    assert(unit_supported("getty@tty1.service") == 0);
    assert(unit_supported("foo.socket") == 0);
    assert(unit_supported("foo.timer") == 0);
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
    fclose(f);
    chmod(p, 0755);
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
    return 0;
}
