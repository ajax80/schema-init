#include "../schema-udev.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_coldplug_events = 0;
static void test_handler(const struct uevent *ev) {
    if (strcmp(uevent_get(ev, "ACTION"), "add") == 0) g_coldplug_events++;
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
    return 0;
}
