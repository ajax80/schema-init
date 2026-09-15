#include "../systemctl_shim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

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

int main(void) {
    test_strip_suffix();
    test_supported();
    printf("task1 systemctl-shim tests passed\n");
    test_enable_queue();
    printf("task2 systemctl-shim tests passed\n");
    return 0;
}
