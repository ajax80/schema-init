#include "../udev_ruleset.h"
#include "../udev_builtins.h"
#include "../udev_exec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>

static void ue_set(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

static void test_result_subst(void) {
    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add"); ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);
    assert(ctx.result[0] == '\0');
    safe_copy(ctx.result, "alpha beta gamma", sizeof ctx.result);
    char out[UE_VAL_MAX];
    ruleset_subst("$result", &ctx, out, sizeof out);   assert(!strcmp(out, "alpha beta gamma"));
    ruleset_subst("%c", &ctx, out, sizeof out);         assert(!strcmp(out, "alpha beta gamma"));
    ruleset_subst("%c{2}", &ctx, out, sizeof out);      assert(!strcmp(out, "beta"));
    ruleset_subst("$result{3}", &ctx, out, sizeof out); assert(!strcmp(out, "gamma"));
    ruleset_subst("%c{9}", &ctx, out, sizeof out);      assert(!strcmp(out, ""));
    printf("test_udev_r4b: result-subst OK\n");
}

static void test_argv_split(void) {
    char store[16][UE_VAL_MAX]; char *argv[17];
    int n = udev_argv_split("ata_id --export /dev/sda", store, argv, 16);
    assert(n == 3);
    assert(!strcmp(argv[0], "ata_id")); assert(!strcmp(argv[1], "--export"));
    assert(!strcmp(argv[2], "/dev/sda")); assert(argv[3] == NULL);

    n = udev_argv_split("/bin/sh -c 'logger hi there' -- x", store, argv, 16);
    assert(n == 5);
    assert(!strcmp(argv[0], "/bin/sh")); assert(!strcmp(argv[1], "-c"));
    assert(!strcmp(argv[2], "logger hi there"));
    assert(!strcmp(argv[3], "--")); assert(!strcmp(argv[4], "x"));
    printf("test_udev_r4b: argv-split OK\n");
}

static void test_run_capture(void) {
    char dir[] = "/tmp/r4b_execXXXXXX"; assert(mkdtemp(dir));
    char ok[PATH_MAX]; snprintf(ok, sizeof ok, "%s/ok.sh", dir);
    FILE *f = fopen(ok, "w"); assert(f);
    fprintf(f, "#!/bin/sh\necho 'ID_FOO=bar'\nexit 0\n"); fclose(f);
    assert(chmod(ok, 0755) == 0);

    char bad[PATH_MAX]; snprintf(bad, sizeof bad, "%s/bad.sh", dir);
    f = fopen(bad, "w"); assert(f);
    fprintf(f, "#!/bin/sh\necho nope\nexit 3\n"); fclose(f);
    assert(chmod(bad, 0755) == 0);

    char out[UE_VAL_MAX];
    char cmd[PATH_MAX + 8];
    snprintf(cmd, sizeof cmd, "%s", ok);
    assert(udev_run_capture(cmd, out, sizeof out) == 0);
    assert(!strcmp(out, "ID_FOO=bar\n"));

    snprintf(cmd, sizeof cmd, "%s", bad);
    assert(udev_run_capture(cmd, out, sizeof out) == 3);

    assert(udev_run_capture("/nonexistent/xyz", out, sizeof out) == -1);
    printf("test_udev_r4b: run-capture OK\n");
}

static void test_program_result(void) {
    char dir[] = "/tmp/r4b_prXXXXXX"; assert(mkdtemp(dir));
    char sh[PATH_MAX]; snprintf(sh, sizeof sh, "%s/echo1.sh", dir);
    FILE *f = fopen(sh, "w"); assert(f);
    fprintf(f, "#!/bin/sh\nprintf '%%s' \"$1\"\nexit 0\n"); fclose(f);
    assert(chmod(sh, 0755) == 0);

    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add"); ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);

    struct rule r; char line[PATH_MAX + 64];
    /* PROGRAM stores result; RESULT== matches it; whole rule matches */
    snprintf(line, sizeof line, "PROGRAM==\"%s hello\", RESULT==\"hello\"", sh);
    ruleset_parse_line(line, &r);
    assert(rule_match(&r, &ctx) == 1);
    assert(!strcmp(ctx.result, "hello"));

    /* RESULT mismatch → rule fails */
    snprintf(line, sizeof line, "PROGRAM==\"%s hello\", RESULT==\"world\"", sh);
    ruleset_parse_line(line, &r);
    assert(rule_match(&r, &ctx) == 0);

    /* nonzero exit → rule fails */
    char bad[PATH_MAX]; snprintf(bad, sizeof bad, "%s/bad.sh", dir);
    f = fopen(bad, "w"); assert(f); fprintf(f, "#!/bin/sh\nexit 5\n"); fclose(f);
    assert(chmod(bad, 0755) == 0);
    snprintf(line, sizeof line, "PROGRAM==\"%s\"", bad);
    ruleset_parse_line(line, &r);
    assert(rule_match(&r, &ctx) == 0);
    printf("test_udev_r4b: program-result OK\n");
}

static void test_fido_id(void) {
    char dir[] = "/tmp/r4b_fidoXXXXXX"; assert(mkdtemp(dir));
    char sysdev[PATH_MAX]; snprintf(sysdev, sizeof sysdev, "%s/devices/hid0", dir);
    char cmd[PATH_MAX + 16]; snprintf(cmd, sizeof cmd, "mkdir -p %s", sysdev); assert(system(cmd) == 0);
    char rd[PATH_MAX]; assert((size_t)snprintf(rd, sizeof rd, "%s/report_descriptor", sysdev) < sizeof rd);
    unsigned char desc[] = { 0x06, 0xd0, 0xf1, 0x09, 0x01, 0xa1, 0x01 };
    int fd = open(rd, O_WRONLY | O_CREAT, 0644); assert(fd >= 0);
    assert(write(fd, desc, sizeof desc) == (ssize_t)sizeof desc); close(fd);

    struct uevent out; memset(&out, 0, sizeof out);
    int n = fido_id_build(dir, "/devices/hid0", &out);
    assert(n == 2);
    assert(!strcmp(uevent_get(&out, "ID_FIDO_TOKEN"), "1"));
    assert(!strcmp(uevent_get(&out, "ID_SECURITY_TOKEN"), "1"));

    /* non-FIDO descriptor → nothing */
    unsigned char kbd[] = { 0x05, 0x01, 0x09, 0x06, 0xa1, 0x01 };
    fd = open(rd, O_WRONLY | O_TRUNC, 0644); assert(fd >= 0);
    assert(write(fd, kbd, sizeof kbd) == (ssize_t)sizeof kbd); close(fd);
    memset(&out, 0, sizeof out);
    assert(fido_id_build(dir, "/devices/hid0", &out) == 0);

    /* name→bit map */
    assert(builtin_name_bit("fido_id") == UB_FIDO);
    printf("test_udev_r4b: fido-id OK\n");
}

static void test_import_program_bridge(void) {
    char dir[] = "/tmp/r4b_impXXXXXX"; assert(mkdtemp(dir));
    char sh[PATH_MAX]; snprintf(sh, sizeof sh, "%s/foo_id.sh", dir);
    FILE *f = fopen(sh, "w"); assert(f);
    fprintf(f, "#!/bin/sh\necho 'ID_FOO=bar'\necho 'ID_BAZ=qux'\nexit 0\n"); fclose(f);
    assert(chmod(sh, 0755) == 0);

    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add"); ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);

    struct rule r; char line[PATH_MAX + 64];
    snprintf(line, sizeof line, "IMPORT{program}=\"%s\"", sh);
    ruleset_parse_line(line, &r);
    assert(rule_match(&r, &ctx) == 1);
    apply_rule(&r, &ctx);
    assert(!strcmp(uevent_get(&ev, "ID_FOO"), "bar"));
    assert(!strcmp(uevent_get(&ev, "ID_BAZ"), "qux"));

    /* nonzero exit → gate: a later assignment in the same rule must NOT apply */
    char bad[PATH_MAX]; snprintf(bad, sizeof bad, "%s/bad_id.sh", dir);
    f = fopen(bad, "w"); assert(f); fprintf(f, "#!/bin/sh\nexit 4\n"); fclose(f);
    assert(chmod(bad, 0755) == 0);
    struct uevent ev2; memset(&ev2, 0, sizeof ev2);
    ue_set(&ev2, "ACTION", "add"); ue_set(&ev2, "DEVPATH", "/devices/y");
    struct dev_ctx c2; assert(dev_ctx_init(&c2, &ev2, "/sys") == 0);
    snprintf(line, sizeof line, "IMPORT{program}=\"%s\", ENV{AFTER}=\"1\"", bad);
    ruleset_parse_line(line, &r);
    rule_match(&r, &c2); apply_rule(&r, &c2);
    assert(uevent_get(&ev2, "AFTER") == NULL);   /* gated */
    printf("test_udev_r4b: import-program-bridge OK\n");
}

static void test_import_builtin_args_strip(void) {
    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add"); ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);

    struct rule r;
    /* path_id is a ported builtin; with args, it should strip them before lookup */
    ruleset_parse_line("IMPORT{builtin}=\"path_id --export\", ENV{AFTER}=\"1\"", &r);
    assert(rule_match(&r, &ctx) == 1);
    ctx.last_rule_deferred = 0;
    apply_rule(&r, &ctx);
    /* path_id might fail on non-device context, but should NOT defer due to args */
    assert(ctx.last_rule_deferred == 0);
    printf("test_udev_r4b: import-builtin-args-strip OK\n");
}

int main(void) {
    test_result_subst();
    test_argv_split();
    test_run_capture();
    test_program_result();
    test_fido_id();
    test_import_program_bridge();
    test_import_builtin_args_strip();
    printf("test_udev_r4b: ALL OK\n");
    return 0;
}
