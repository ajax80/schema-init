#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

static void ue_set(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

int main(void) {
    /* Task 1: ctx defaults */
    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add");
    ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);
    assert(strcmp(ctx.dbroot, "/run/udev/data") == 0);
    assert(strcmp(ctx.cmdline_path, "/proc/cmdline") == 0);
    assert(ctx.nruns == 0);
    printf("test_udev_r4a: ctx-defaults OK\n");

    /* Task 2: TEST */
    {
        char dir[] = "/tmp/r4a_testXXXXXX";
        assert(mkdtemp(dir) != NULL);
        char present[PATH_MAX], mode0700[PATH_MAX], mode0600[PATH_MAX];
        snprintf(present, sizeof present, "%s/here", dir);
        snprintf(mode0700, sizeof mode0700, "%s/priv", dir);
        snprintf(mode0600, sizeof mode0600, "%s/partial", dir);
        FILE *f = fopen(present, "w"); assert(f); fclose(f);
        f = fopen(mode0700, "w"); assert(f); fclose(f);
        assert(chmod(mode0700, 0700) == 0);
        f = fopen(mode0600, "w"); assert(f); fclose(f);
        assert(chmod(mode0600, 0600) == 0);

        struct uevent tev; memset(&tev, 0, sizeof tev);
        ue_set(&tev, "ACTION", "add"); ue_set(&tev, "DEVPATH", "/devices/x");
        struct dev_ctx tc; assert(dev_ctx_init(&tc, &tev, "/sys") == 0);

        struct rule r;
        char line[256];
        snprintf(line, sizeof line, "TEST==\"%s\"", present);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 1);   /* exists */

        snprintf(line, sizeof line, "TEST!=\"%s/absent\"", dir);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 1);   /* absent, != */

        snprintf(line, sizeof line, "TEST==\"%s/absent\"", dir);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 0);   /* absent, == */

        snprintf(line, sizeof line, "TEST{0700}==\"%s\"", mode0700);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 1);   /* mode matches */

        snprintf(line, sizeof line, "TEST{0070}==\"%s\"", mode0700);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 0);   /* group bits absent */

        snprintf(line, sizeof line, "TEST{0402}==\"%s\"", mode0600);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 1);   /* any-bit-overlap: 0600 & 0402 = 0400 > 0 */

        snprintf(line, sizeof line, "TEST{0021}==\"%s\"", mode0600);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 0);   /* no overlap: 0600 & 0021 = 0 */

        /* re-gate: a TEST-gated rule no longer flags deferred */
        snprintf(line, sizeof line, "TEST==\"%s\", ENV{X}=\"1\"", present);
        ruleset_parse_line(line, &r);
        tc.last_rule_deferred = 0;
        assert(rule_match(&r, &tc) == 1);
        assert(tc.last_rule_deferred == 0);

        unlink(present); unlink(mode0700); unlink(mode0600); rmdir(dir);
    }
    printf("test_udev_r4a: TEST OK\n");
    return 0;
}
