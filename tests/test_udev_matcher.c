#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

static void ue_set(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

int main(void) {
    /* alternation */
    assert(udev_glob("sd*|vd*", "sda") == 1);
    assert(udev_glob("sd*|vd*", "vdb") == 1);
    assert(udev_glob("sd*|vd*", "hda") == 0);
    /* globs delegated to fnmatch */
    assert(udev_glob("sd[a-c]", "sdb") == 1);
    assert(udev_glob("sd[a-c]", "sdd") == 0);
    assert(udev_glob("tty?", "ttyS") == 1);
    assert(udev_glob("event[0-9]", "event3") == 1);
    /* a '|' inside a bracket class is NOT an alternation split */
    assert(udev_glob("a[b|c]d", "abd") == 1);
    assert(udev_glob("a[b|c]d", "a|d") == 1);
    /* exact */
    assert(udev_glob("exact", "exact") == 1);
    assert(udev_glob("exact", "other") == 0);

    /* pi_driver: create <dir>/driver symlink, expect basename */
    char t2[] = "/tmp/schema-m2-XXXXXX"; assert(mkdtemp(t2));
    char dvd[128]; snprintf(dvd, sizeof dvd, "%s/dev", t2); assert(mkdir(dvd, 0755) == 0);
    char dl[160]; snprintf(dl, sizeof dl, "%s/driver", dvd);
    assert(symlink("../../bus/pci/drivers/ahci", dl) == 0);
    char drv[64]; assert(pi_driver(dvd, drv, sizeof drv) == 0 && strcmp(drv, "ahci") == 0);
    char dvd2[128]; snprintf(dvd2, sizeof dvd2, "%s/nodrv", t2); assert(mkdir(dvd2, 0755) == 0);
    assert(pi_driver(dvd2, drv, sizeof drv) == -1);

    /* dev_ctx_init: sysdir = sysroot + DEVPATH */
    struct uevent ev2; memset(&ev2, 0, sizeof ev2);
    ue_set(&ev2, "ACTION", "add");
    ue_set(&ev2, "DEVPATH", "/devices/pci/block/sda");
    struct dev_ctx ctx2;
    assert(dev_ctx_init(&ctx2, &ev2, "/sys") == 0);
    assert(strcmp(ctx2.sysdir, "/sys/devices/pci/block/sda") == 0);
    assert(ctx2.ntags == 0 && ctx2.matched_parent[0] == '\0' && ctx2.ev == &ev2);
    struct uevent ev3; memset(&ev3, 0, sizeof ev3); ue_set(&ev3, "ACTION", "add");
    assert(dev_ctx_init(&ctx2, &ev3, "/sys") == -1);   /* no DEVPATH */
    unlink(dl); rmdir(dvd); rmdir(dvd2); rmdir(t2);

    struct uevent evs; memset(&evs, 0, sizeof evs);
    ue_set(&evs, "ACTION", "add");
    ue_set(&evs, "DEVPATH", "/devices/pci/ata1/block/sda/sda3");
    ue_set(&evs, "MAJOR", "8");
    ue_set(&evs, "MINOR", "3");
    ue_set(&evs, "ID_BUS", "ata");
    struct dev_ctx cs; assert(dev_ctx_init(&cs, &evs, "/sys") == 0);
    char o[256];

    ruleset_subst("k=%k n=%n M=%M m=%m", &cs, o, sizeof o);
    assert(strcmp(o, "k=sda3 n=3 M=8 m=3") == 0);
    ruleset_subst("$env{ID_BUS}-$kernel", &cs, o, sizeof o);
    assert(strcmp(o, "ata-sda3") == 0);
    ruleset_subst("p=$devpath", &cs, o, sizeof o);
    assert(strcmp(o, "p=/devices/pci/ata1/block/sda/sda3") == 0);
    ruleset_subst("100%%$$done", &cs, o, sizeof o);
    assert(strcmp(o, "100%$done") == 0);
    /* deferred tokens copied verbatim */
    ruleset_subst("x$result-$links-%c-$name", &cs, o, sizeof o);
    assert(strcmp(o, "x$result-$links-%c-$name") == 0);
    /* $id / %b reads matched_parent */
    safe_copy(cs.matched_parent, "0000:00:1f.2", sizeof cs.matched_parent);
    ruleset_subst("$id|%b", &cs, o, sizeof o);
    assert(strcmp(o, "0000:00:1f.2|0000:00:1f.2") == 0);
    /* sz==0: no crash, no out-of-bounds write */
    char z[1] = {0x7f};
    ruleset_subst("$$", &cs, z, 0);
    assert(z[0] == 0x7f);

    printf("test_udev_matcher: subst OK\n");
    printf("test_udev_matcher: glob OK\n");
    printf("test_udev_matcher: ctx OK\n");

    /* device-level matching, incl. an ATTR read from a synthetic sysdir */
    char t4[] = "/tmp/schema-m4-XXXXXX"; assert(mkdtemp(t4));
    char xdir[PATH_MAX]; snprintf(xdir, sizeof xdir, "%s/devices", t4); assert(mkdir(xdir, 0755) == 0);
    snprintf(xdir, sizeof xdir, "%s/devices/sda", t4); assert(mkdir(xdir, 0755) == 0);
    char af[PATH_MAX]; snprintf(af, sizeof af, "%s/devices/sda/serial", t4);
    FILE *sf = fopen(af, "w"); fputs("ABC123\n", sf); fclose(sf);

    struct uevent evm; memset(&evm, 0, sizeof evm);
    ue_set(&evm, "ACTION", "add");
    ue_set(&evm, "DEVPATH", "/devices/sda");
    ue_set(&evm, "SUBSYSTEM", "block");
    ue_set(&evm, "DRIVER", "sd");
    ue_set(&evm, "ID_FS_TYPE", "ext4");
    struct dev_ctx cm; assert(dev_ctx_init(&cm, &evm, t4) == 0);
    safe_copy(cm.tags[cm.ntags++], "systemd", UE_KEY_MAX);

    struct rule r;
    ruleset_parse_line("ACTION==\"add\", SUBSYSTEM==\"block\", KERNEL==\"sd*\"", &r);
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("SUBSYSTEM==\"net\"", &r);
    assert(rule_match(&r, &cm) == 0);
    ruleset_parse_line("ACTION!=\"remove\", ENV{ID_FS_TYPE}==\"ext4\"", &r);
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("ENV{NOPE}==\"x\"", &r);          /* missing => == fails */
    assert(rule_match(&r, &cm) == 0);
    ruleset_parse_line("ENV{NOPE}!=\"x\"", &r);          /* missing => != passes */
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("ATTR{serial}==\"ABC123\"", &r);  /* sysfs attr read */
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("ATTR{serial}==\"WRONG\"", &r);
    assert(rule_match(&r, &cm) == 0);
    ruleset_parse_line("TAG==\"systemd\"", &r);
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("TAG==\"seat\"", &r);
    assert(rule_match(&r, &cm) == 0);
    /* assignment clauses are ignored by the matcher */
    ruleset_parse_line("SUBSYSTEM==\"block\", SYMLINK+=\"disk/by-x\"", &r);
    assert(rule_match(&r, &cm) == 1);

    unlink(af); rmdir(xdir);
    snprintf(xdir, sizeof xdir, "%s/devices", t4); rmdir(xdir); rmdir(t4);

    printf("test_udev_matcher: dev-match OK\n");
    return 0;
}
