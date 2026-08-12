#include "../udev_ruleset.h"
#include "../udev_builtins.h"
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
        char line[PATH_MAX + 64];
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

    /* Task 3: run_builtin_bit exists and is a no-op on a bogus device */
    {
        struct uevent bev; memset(&bev, 0, sizeof bev);
        ue_set(&bev, "ACTION", "add"); ue_set(&bev, "DEVPATH", "/devices/none");
        int before = bev.n;
        int rc = run_builtin_bit("/nonexistent-sysroot", "/devices/none", NULL, &bev, UB_USB);
        assert(rc < 0);            /* usb_id on a non-USB/absent device fails */
        assert(bev.n == before);   /* nothing absorbed */
    }
    printf("test_udev_r4a: run_builtin_bit OK\n");

    /* Task 3: run_builtins actually executes the loop, and the corrected
     * V4L/ATA/CDROM mappings behave as derived, on a bogus/absent device.
     * devnode is a nonexistent-but-non-NULL path here: a literal NULL at these
     * multiple sibling call sites lets -O2 constant-propagate a specialized
     * run_builtin_bit clone in which the UB_BLKID branch (which does not
     * NULL-check devnode before calling open()) is flagged -Wnonnull, even
     * though none of these calls select that bit. */
    {
        const char *bogus_devnode = "/dev/nonexistent-r4a-test";

        struct uevent gev; memset(&gev, 0, sizeof gev);
        ue_set(&gev, "ACTION", "add"); ue_set(&gev, "DEVPATH", "/devices/none");
        int before = gev.n;
        int rc = run_builtins("/nonexistent-sysroot", "/devices/none", bogus_devnode, &gev);
        assert(rc == 0);
        assert(gev.n == before);

        struct uevent mev; memset(&mev, 0, sizeof mev);
        ue_set(&mev, "ACTION", "add"); ue_set(&mev, "DEVPATH", "/devices/none");
        int mbefore = mev.n;
        assert(run_builtin_bit("/nonexistent-sysroot", "/devices/none", bogus_devnode, &mev, UB_V4L) < 0);
        assert(mev.n == mbefore);
        assert(run_builtin_bit("/nonexistent-sysroot", "/devices/none", bogus_devnode, &mev, UB_ATA) < 0);
        assert(mev.n == mbefore);
        assert(run_builtin_bit("/nonexistent-sysroot", "/devices/none", bogus_devnode, &mev, UB_CDROM) == 0);
        assert(mev.n == mbefore);
    }
    printf("test_udev_r4a: run_builtins-guard OK\n");

    /* Task 4: IMPORT{builtin} gate semantics */
    {
        struct uevent gev; memset(&gev, 0, sizeof gev);
        ue_set(&gev, "ACTION", "add"); ue_set(&gev, "DEVPATH", "/devices/none");
        struct dev_ctx gc; assert(dev_ctx_init(&gc, &gev, "/nonexistent-sysroot") == 0);

        /* ported builtin that FAILS -> hard gate: later ENV assignment must NOT apply */
        struct rule r;
        ruleset_parse_line("IMPORT{builtin}=\"usb_id\", ENV{AFTER}=\"1\"", &r);
        assert(rule_match(&r, &gc) == 1);
        apply_rule(&r, &gc);
        assert(uevent_get(gc.ev, "AFTER") == NULL);

        /* un-ported builtin -> deferred, NOT a gate: later ENV assignment DOES apply */
        struct uevent dev2; memset(&dev2, 0, sizeof dev2);
        ue_set(&dev2, "ACTION", "add"); ue_set(&dev2, "DEVPATH", "/devices/none");
        struct dev_ctx dc; assert(dev_ctx_init(&dc, &dev2, "/sys") == 0);
        ruleset_parse_line("IMPORT{builtin}=\"keyboard\", ENV{AFTER}=\"1\"", &r);
        assert(rule_match(&r, &dc) == 1);
        dc.last_rule_deferred = 0;
        apply_rule(&r, &dc);
        assert(strcmp(uevent_get(dc.ev, "AFTER"), "1") == 0);
        assert(dc.last_rule_deferred == 1);

        /* IMPORT{program} -> deferred, NOT a gate */
        struct uevent pev; memset(&pev, 0, sizeof pev);
        ue_set(&pev, "ACTION", "add"); ue_set(&pev, "DEVPATH", "/devices/none");
        struct dev_ctx pc; assert(dev_ctx_init(&pc, &pev, "/sys") == 0);
        ruleset_parse_line("IMPORT{program}=\"/bin/true\", ENV{AFTER}=\"1\"", &r);
        pc.last_rule_deferred = 0;
        apply_rule(&r, &pc);
        assert(strcmp(uevent_get(pc.ev, "AFTER"), "1") == 0);
        assert(pc.last_rule_deferred == 1);

        /* deferred bump is counted once by ruleset_apply, after apply */
        struct uevent sev; memset(&sev, 0, sizeof sev);
        ue_set(&sev, "ACTION", "add"); ue_set(&sev, "DEVPATH", "/devices/none");
        struct dev_ctx sc2; assert(dev_ctx_init(&sc2, &sev, "/sys") == 0);
        struct ruleset rs = {0};
        struct rule sr;
        assert(ruleset_parse_line("IMPORT{builtin}=\"keyboard\"", &sr) > 0);
        assert(ruleset_append(&rs, &sr) == 0);
        ruleset_apply(&rs, &sc2);
        assert(sc2.deferred_applies == 1);
        free(rs.rules);
    }
    printf("test_udev_r4a: IMPORT-gate OK\n");

    /* Task 4: blkid builtins must not crash on a NULL devnode (no DEVNAME) */
    {
        char dir[] = "/tmp/r4a_blkidXXXXXX";
        assert(mkdtemp(dir) != NULL);
        char childpath[sizeof dir + 8];
        snprintf(childpath, sizeof childpath, "%s/child", dir);
        assert(mkdir(childpath, 0700) == 0);
        char partfile[sizeof dir + 20];
        snprintf(partfile, sizeof partfile, "%s/child/partition", dir);
        FILE *f = fopen(partfile, "w"); assert(f); fprintf(f, "1\n"); fclose(f);

        struct uevent bev; memset(&bev, 0, sizeof bev);
        ue_set(&bev, "ACTION", "add"); ue_set(&bev, "DEVPATH", "/child");
        int before = bev.n;
        int rc = run_builtin_bit(dir, "/child", NULL, &bev, UB_BLKID);
        assert(rc == 0);
        assert(bev.n == before);

        unlink(partfile); rmdir(childpath); rmdir(dir);
    }
    printf("test_udev_r4a: blkid-null-devnode OK\n");

    /* Task 5: IMPORT{cmdline} */
    {
        char cf[] = "/tmp/r4a_cmdlineXXXXXX";
        int fd = mkstemp(cf); assert(fd >= 0);
        dprintf(fd, "quiet root=/dev/sda2 rd.foo=bar\n"); close(fd);

        struct uevent cev; memset(&cev, 0, sizeof cev);
        ue_set(&cev, "ACTION", "add"); ue_set(&cev, "DEVPATH", "/devices/x");
        struct dev_ctx cc; assert(dev_ctx_init(&cc, &cev, "/sys") == 0);
        cc.cmdline_path = cf;

        import_cmdline(&cc, "rd.foo");
        assert(strcmp(uevent_get(cc.ev, "rd.foo"), "bar") == 0);
        import_cmdline(&cc, "quiet");
        assert(strcmp(uevent_get(cc.ev, "quiet"), "1") == 0);   /* bare flag -> "1" */
        import_cmdline(&cc, "absent");
        assert(uevent_get(cc.ev, "absent") == NULL);            /* soft: no-op */
        unlink(cf);
    }
    printf("test_udev_r4a: IMPORT-cmdline OK\n");

    /* Task 6: IMPORT{db} */
    {
        char dbdir[] = "/tmp/r4a_dbXXXXXX"; assert(mkdtemp(dbdir));
        char rec[PATH_MAX]; snprintf(rec, sizeof rec, "%s/b8:0", dbdir);
        FILE *f = fopen(rec, "w"); assert(f);
        fprintf(f, "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=dead-beef\nS:disk/by-uuid/dead-beef\n");
        fclose(f);

        struct uevent dev; memset(&dev, 0, sizeof dev);
        ue_set(&dev, "ACTION", "add"); ue_set(&dev, "DEVPATH", "/devices/virtual/block/sda");
        ue_set(&dev, "SUBSYSTEM", "block"); ue_set(&dev, "MAJOR", "8"); ue_set(&dev, "MINOR", "0");
        ue_set(&dev, "DEVNAME", "sda");
        struct dev_ctx dc; assert(dev_ctx_init(&dc, &dev, "/sys") == 0);
        dc.dbroot = dbdir;

        import_db(&dc, "ID_FS_TYPE");
        assert(strcmp(uevent_get(dc.ev, "ID_FS_TYPE"), "ext4") == 0);
        import_db(&dc, "ID_FS_UUID");
        assert(strcmp(uevent_get(dc.ev, "ID_FS_UUID"), "dead-beef") == 0);
        import_db(&dc, "NOPE");
        assert(uevent_get(dc.ev, "NOPE") == NULL);       /* missing key: no-op */
        unlink(rec); rmdir(dbdir);

        /* missing file: no-op, no crash */
        dc.dbroot = "/tmp/r4a_absent_db";
        import_db(&dc, "ID_FS_TYPE");                    /* still ext4 from before, unchanged */
    }
    printf("test_udev_r4a: IMPORT-db OK\n");

    /* Task 7: IMPORT{parent} */
    {
        char root[] = "/tmp/r4a_sysXXXXXX"; assert(mkdtemp(root));
        /* child: <root>/devices/pci/blk/sda1 ; parent: .../blk (block, b8:0) */
        char parent[PATH_MAX], child[PATH_MAX];
        snprintf(parent, sizeof parent, "%s/devices/pci/blk", root);
        snprintf(child,  sizeof child,  "%s/devices/pci/blk/sda1", root);
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", child); assert(system(cmd) == 0);
        /* parent uevent gives MAJOR/MINOR so its db filename is b8:0 */
        char uev[PATH_MAX]; assert((size_t)snprintf(uev, sizeof uev, "%s/uevent", parent) < sizeof uev);
        FILE *f = fopen(uev, "w"); assert(f);
        fprintf(f, "MAJOR=8\nMINOR=0\nDEVNAME=sda\nSUBSYSTEM=block\n"); fclose(f);

        char dbdir[] = "/tmp/r4a_pdbXXXXXX"; assert(mkdtemp(dbdir));
        char rec[PATH_MAX]; snprintf(rec, sizeof rec, "%s/b8:0", dbdir);
        f = fopen(rec, "w"); assert(f);
        fprintf(f, "E:ID_SERIAL=WDC-123\nE:ID_MODEL=WDC\nE:OTHER=x\n"); fclose(f);

        struct uevent dev; memset(&dev, 0, sizeof dev);
        ue_set(&dev, "ACTION", "add");
        ue_set(&dev, "DEVPATH", "/devices/pci/blk/sda1");
        ue_set(&dev, "SUBSYSTEM", "block");
        struct dev_ctx dc; assert(dev_ctx_init(&dc, &dev, root) == 0);
        dc.dbroot = dbdir;

        import_parent(&dc, "ID_*");
        assert(strcmp(uevent_get(dc.ev, "ID_SERIAL"), "WDC-123") == 0);
        assert(strcmp(uevent_get(dc.ev, "ID_MODEL"), "WDC") == 0);
        assert(uevent_get(dc.ev, "OTHER") == NULL);     /* glob did not match */

        snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", root, dbdir); assert(system(cmd) == 0);
    }
    printf("test_udev_r4a: IMPORT-parent OK\n");
    return 0;
}
