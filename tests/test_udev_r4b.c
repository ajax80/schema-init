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

int main(void) {
    test_result_subst();
    test_argv_split();
    test_run_capture();
    test_program_result();
    printf("test_udev_r4b: ALL OK\n");
    return 0;
}
