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

    printf("test_udev_matcher: glob OK\n");
    printf("test_udev_matcher: ctx OK\n");
    return 0;
}
