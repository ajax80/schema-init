#include "../schema-udev.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_coldplug_events = 0;
static void test_handler(struct uevent *ev) {
    if (strcmp(uevent_get(ev, "ACTION"), "add") == 0) g_coldplug_events++;
}

static char g_order[16][64];
static int g_order_n = 0;
static void order_handler(struct uevent *ev) {
    const char *dn = uevent_get(ev, "DEVNAME");
    if (dn && g_order_n < 16) { safe_copy(g_order[g_order_n], dn, 64); g_order_n++; }
}

int main(void) {
    char tmpl[] = "/tmp/schema-udev-test-sysXXXXXX";
    char *sysroot = mkdtemp(tmpl);
    assert(sysroot);

    /* Build fake sysfs path: <sysroot>/devices/pci0000:00/0000:00:14.0/usb1/1-4/1-4:1.0/ttyUSB0 */
    char devdir[1024];
    snprintf(devdir, sizeof devdir, "%s/devices/pci0000:00/0000:00:14.0/usb1/1-4/1-4:1.0/ttyUSB0", sysroot);
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", devdir);
    assert(system(cmd) == 0);

    /* Create subsystem symlink -> .../class/tty */
    char subtarget[1024], sublink[1024];
    snprintf(subtarget, sizeof subtarget, "%s/class/tty", sysroot);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", subtarget);
    assert(system(cmd) == 0);
    strcpy(sublink, devdir); strcat(sublink, "/subsystem");
    assert(symlink(subtarget, sublink) == 0);

    /* Create uevent file */
    char uevent_file[1024];
    strcpy(uevent_file, devdir); strcat(uevent_file, "/uevent");
    FILE *f = fopen(uevent_file, "w");
    assert(f);
    fputs("MAJOR=188\nMINOR=0\nDEVNAME=ttyUSB0\nPRODUCT=10c4/ea60/100\n", f);
    fclose(f);

    /* Synthesize struct uevent */
    struct uevent ev;
    assert(uevent_from_sysfs(sysroot, devdir, &ev) == 0);

    assert(strcmp(uevent_get(&ev, "ACTION"), "add") == 0);
    assert(strcmp(uevent_get(&ev, "DEVPATH"), "/devices/pci0000:00/0000:00:14.0/usb1/1-4/1-4:1.0/ttyUSB0") == 0);
    assert(strcmp(uevent_get(&ev, "SUBSYSTEM"), "tty") == 0);
    assert(strcmp(uevent_get(&ev, "DEVNAME"), "ttyUSB0") == 0);
    assert(strcmp(uevent_get(&ev, "MAJOR"), "188") == 0);
    assert(strcmp(uevent_get(&ev, "PRODUCT"), "10c4/ea60/100") == 0);

    /* Assert matching rule fires */
    struct dev_rule r; memset(&r, 0, sizeof r);
    dev_rule_set(&r, "name", "esp32");
    dev_rule_set(&r, "match_subsystem", "tty");
    dev_rule_set(&r, "match_product", "10c4/*");
    dev_rule_set(&r, "on_add", "/bin/true");
    assert(dev_rule_match(&r, &ev) == 1);

    printf("test_coldplug (uevent_from_sysfs): OK\n");

    /* Test coldplug_walk_root */
    char tmpl2[] = "/tmp/schema-udev-test-walkXXXXXX";
    char *sysroot2 = mkdtemp(tmpl2);
    assert(sysroot2);

    char d1[1024], d2[1024];
    snprintf(d1, sizeof d1, "%s/devices/dev1", sysroot2);
    snprintf(d2, sizeof d2, "%s/devices/dev2", sysroot2);
    char devroot2[1024];
    snprintf(devroot2, sizeof devroot2, "%s/devices", sysroot2);
    mkdir(devroot2, 0755);
    mkdir(d1, 0755);
    mkdir(d2, 0755);

    char u1[1024], u2[1024];
    strcpy(u1, d1); strcat(u1, "/uevent"); FILE *f1 = fopen(u1, "w"); fputs("DEVNAME=dev1\n", f1); fclose(f1);
    strcpy(u2, d2); strcat(u2, "/uevent"); FILE *f2 = fopen(u2, "w"); fputs("DEVNAME=dev2\n", f2); fclose(f2);

    g_coldplug_events = 0;
    assert(coldplug_walk_root(sysroot2, test_handler) == 0);
    assert(g_coldplug_events == 2);

    printf("test_coldplug (coldplug_walk_root): OK\n");

    /* Ordering contract: a parent device is always dispatched before its
     * children (its devpath is a prefix). Build child subtree + uevent FIRST
     * to bias readdir toward child-first, then assert coldplug still orders
     * parent before child. */
    char tmpl3[] = "/tmp/schema-udev-test-ordXXXXXX";
    char *sr3 = mkdtemp(tmpl3); assert(sr3);
    char pdir[1024], cdir[1024];
    snprintf(pdir, sizeof pdir, "%s/devices/blk/sda", sr3);
    snprintf(cdir, sizeof cdir, "%s/devices/blk/sda/sda1", sr3);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", cdir); assert(system(cmd) == 0);
    char cu[1100], pu[1100];
    snprintf(cu, sizeof cu, "%s/uevent", cdir); f = fopen(cu, "w"); fputs("DEVNAME=sda1\n", f); fclose(f);
    snprintf(pu, sizeof pu, "%s/uevent", pdir); f = fopen(pu, "w"); fputs("DEVNAME=sda\n", f); fclose(f);

    g_order_n = 0;
    assert(coldplug_walk_root(sr3, order_handler) == 0);
    assert(g_order_n == 2);
    int i_parent = -1, i_child = -1;
    for (int i = 0; i < g_order_n; i++) {
        if (!strcmp(g_order[i], "sda"))  i_parent = i;
        if (!strcmp(g_order[i], "sda1")) i_child  = i;
    }
    assert(i_parent >= 0 && i_child >= 0);
    assert(i_parent < i_child);   /* parent before child, guaranteed */

    printf("test_coldplug (parent-before-child order): OK\n");

    /* readiness marker: created after coldplug so schema-init's ready_path can
       gate services that depend on the device manager (network-up etc). */
    char tmpl4[] = "/tmp/schema-udev-ready-XXXXXX";
    char *rd = mkdtemp(tmpl4); assert(rd);
    char sub[512]; snprintf(sub, sizeof sub, "%s/run", rd);   /* dir does not exist yet */
    assert(udev_signal_ready_at(sub) == 0);
    char marker[600]; snprintf(marker, sizeof marker, "%s/ready", sub);
    struct stat rst; assert(stat(marker, &rst) == 0 && S_ISREG(rst.st_mode));
    assert(udev_signal_ready_at(sub) == 0);                   /* idempotent */
    printf("test_coldplug (ready-marker): OK\n");
    return 0;
}
