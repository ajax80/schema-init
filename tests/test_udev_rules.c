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

/* composite usb modalias is built as usb:vVVVVpPPPP from idVendor/idProduct */
static void test_composite_usb_modalias(void) {
    char out[128];
    assert(rules_usb_modalias("1d6b", "0002", NULL, out, sizeof out) == 0);
    assert(strcmp(out, "usb:v1D6Bp0002") == 0);
    /* with bcdDevice -> appends dVVVV */
    assert(rules_usb_modalias("1d6b", "0002", "0410", out, sizeof out) == 0);
    assert(strcmp(out, "usb:v1D6Bp0002d0410") == 0);
    printf("test_udev_rules composite usb modalias: OK\n");
}

/* OUI lookup key from a MAC address is OUI:XXXXXX (first 3 octets, upper, no colons) */
static void test_composite_oui_key(void) {
    char out[32];
    assert(rules_oui_key("00:1a:2b:3c:4d:5e", out, sizeof out) == 0);
    assert(strcmp(out, "OUI:001A2B") == 0);
    printf("test_udev_rules composite OUI key: OK\n");
}

/* usb_interface keys usb_id skips (INTERFACE_NUM/DRIVER/TYPE/ID_TYPE) are
 * synthesized on the interface and inherited by an input child. */
static void test_usb_interface_inherit(void) {
    root_make();
    mkdirs("/devices/pci0/usb1/1-2/1-2:1.0");
    writef("/devices/pci0/usb1/1-2/uevent", "DEVTYPE=usb_device\n");
    writef("/devices/pci0/usb1/1-2/1-2:1.0/uevent", "DEVTYPE=usb_interface\n");
    writef("/devices/pci0/usb1/1-2/1-2:1.0/bInterfaceNumber", "01\n");
    writef("/devices/pci0/usb1/1-2/1-2:1.0/bInterfaceClass", "03\n");
    set_subsystem("/devices/pci0", "pci");
    set_subsystem("/devices/pci0/usb1", "usb");
    set_subsystem("/devices/pci0/usb1/1-2", "usb");
    set_subsystem("/devices/pci0/usb1/1-2/1-2:1.0", "usb");
    { char lp[PATH_MAX], tgt[PATH_MAX];             /* interface driver symlink */
      snprintf(lp, sizeof lp, "%s/devices/pci0/usb1/1-2/1-2:1.0/driver", ROOT);
      snprintf(tgt, sizeof tgt, "%s/bus/usb/drivers/usbhid", ROOT);
      mkdirs("/bus/usb/drivers/usbhid"); assert(symlink(tgt, lp) == 0); }
    mkdirs("/devices/pci0/usb1/1-2/1-2:1.0/input/input9");
    writef("/devices/pci0/usb1/1-2/1-2:1.0/input/input9/uevent", "DEVTYPE=\n");
    set_subsystem("/devices/pci0/usb1/1-2/1-2:1.0/input/input9", "input");

    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "input"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");
    strcpy(ev.val[ev.n], "/devices/pci0/usb1/1-2/1-2:1.0/input/input9"); ev.n++;
    run_rules(ROOT, ev.val[1], NULL, &ev);
    assert(strcmp(uevent_get(&ev, "ID_USB_INTERFACE_NUM"), "01") == 0);
    assert(strcmp(uevent_get(&ev, "ID_USB_DRIVER"), "usbhid") == 0);
    assert(strcmp(uevent_get(&ev, "ID_USB_TYPE"), "hid") == 0);
    assert(strcmp(uevent_get(&ev, "ID_TYPE"), "hid") == 0);
    printf("test_udev_rules usb_interface inherit: OK\n");
}

int main(void) {
    test_inert_on_childless();
    test_inherit_id_path();
    test_inherit_first_writer_wins();
    test_inherit_bounded_set();
    test_composite_usb_modalias();
    test_composite_oui_key();
    test_usb_interface_inherit();
    return 0;
}
