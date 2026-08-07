#include "udev_rules.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- synthetic sysfs builder (same pattern as test_udev_builtins.c) --- */
static char ROOT[64];
static void root_make(void) { strcpy(ROOT, "/tmp/urtestXXXXXX"); assert(mkdtemp(ROOT)); }
static void mkdirs(const char *rel) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    for (char *s = p + strlen(ROOT) + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static void writef(const char *rel, const char *body) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    FILE *f = fopen(p, "w"); assert(f); fputs(body, f); fclose(f);
}
static void set_subsystem(const char *devrel, const char *name) {
    char busrel[PATH_MAX]; snprintf(busrel, sizeof busrel, "/bus/%s", name); mkdirs(busrel);
    char linkp[PATH_MAX], target[PATH_MAX];
    snprintf(linkp, sizeof linkp, "%s%s/subsystem", ROOT, devrel);
    snprintf(target, sizeof target, "%s/bus/%s", ROOT, name);
    unlink(linkp); assert(symlink(target, linkp) == 0);
}
static const char *getval(const struct uevent *ev, const char *k) { return uevent_get(ev, k); }

static void test_inert_on_childless(void) {
    root_make();
    /* a lone device with a uevent file and no interesting ancestors/modalias */
    mkdirs("/devices/virtual/mem/null");
    writef("/devices/virtual/mem/null/uevent", "DEVTYPE=\nMAJOR=1\nMINOR=3\n");
    set_subsystem("/devices/virtual/mem/null", "mem");
    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "mem"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");   strcpy(ev.val[ev.n], "/devices/virtual/mem/null"); ev.n++;
    int before = ev.n;
    int added = run_rules(ROOT, "/devices/virtual/mem/null", "/dev/null", &ev);
    assert(added == 0);
    assert(ev.n == before);
    (void)getval;
    printf("test_udev_rules inert: OK\n");
}

/* child (hidraw) under a usb_device parent that carries ID_USB_VENDOR-style props.
 * We seed the PARENT with a modalias so hwdb/usb-derived keys exist to inherit,
 * and assert the child inherits ID_PATH from the ancestor chain. */
static void test_inherit_id_path(void) {
    root_make();
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/uevent",
           "DEVTYPE=usb_device\n");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/modalias", "usb:v1234p5678\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0", "pci");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1", "usb");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2", "usb");
    /* child hidraw device */
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/hidraw/hidraw5");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/hidraw/hidraw5/uevent",
           "DEVTYPE=\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/hidraw/hidraw5", "hidraw");

    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "hidraw"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");
    strcpy(ev.val[ev.n], "/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/hidraw/hidraw5");
    ev.n++;
    run_rules(ROOT, ev.val[1], NULL, &ev);
    const char *idp = uevent_get(&ev, "ID_PATH");
    assert(idp != NULL);            /* inherited from the usb ancestor's path_id */
    printf("test_udev_rules inherit ID_PATH: OK\n");
}

/* first-writer-wins: a child that already has its own ID_PATH keeps it */
static void test_inherit_first_writer_wins(void) {
    root_make();
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/uevent", "DEVTYPE=usb_device\n");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/modalias", "usb:v1234p5678\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0", "pci");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1", "usb");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2", "usb");
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child/uevent", "DEVTYPE=\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child", "hidraw");

    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "hidraw"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");
    strcpy(ev.val[ev.n], "/devices/pci0000:00/0000:00:14.0/usb1/1-2/child"); ev.n++;
    strcpy(ev.key[ev.n], "ID_PATH"); strcpy(ev.val[ev.n], "MINE-DO-NOT-REPLACE"); ev.n++;
    run_rules(ROOT, ev.val[1], NULL, &ev);
    assert(strcmp(uevent_get(&ev, "ID_PATH"), "MINE-DO-NOT-REPLACE") == 0);
    printf("test_udev_rules first-writer-wins: OK\n");
}

/* a non-inheritable key on the ancestor is NOT copied down */
static void test_inherit_bounded_set(void) {
    root_make();
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/uevent",
           "DEVTYPE=usb_device\nID_NONINHERIT=nope\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0", "pci");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1", "usb");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2", "usb");
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child/uevent", "DEVTYPE=\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child", "hidraw");

    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "hidraw"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");
    strcpy(ev.val[ev.n], "/devices/pci0000:00/0000:00:14.0/usb1/1-2/child"); ev.n++;
    run_rules(ROOT, ev.val[1], NULL, &ev);
    assert(uevent_get(&ev, "ID_NONINHERIT") == NULL);
    printf("test_udev_rules bounded-set: OK\n");
}

int main(void) {
    test_inert_on_childless();
    test_inherit_id_path();
    test_inherit_first_writer_wins();
    test_inherit_bounded_set();
    return 0;
}
