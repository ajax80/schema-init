#include "udev_builtins.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- synthetic sysfs builder rooted at a mkdtemp dir --- */
static char ROOT[64];
static void root_make(void) { strcpy(ROOT, "/tmp/ubtestXXXXXX"); assert(mkdtemp(ROOT)); }
static void mkdirs(const char *rel) {                 /* mkdir -p ROOT/rel */
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    for (char *s = p + strlen(ROOT) + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static void writef(const char *rel, const char *body) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    FILE *f = fopen(p, "w"); assert(f); fputs(body, f); fclose(f);
}
/* create ROOT/<devrel>/subsystem -> a dir whose basename is `name` */
static void set_subsystem(const char *devrel, const char *name) {
    char busrel[PATH_MAX]; snprintf(busrel, sizeof busrel, "/bus/%s", name); mkdirs(busrel);
    char linkp[PATH_MAX], target[PATH_MAX];
    snprintf(linkp, sizeof linkp, "%s%s/subsystem", ROOT, devrel);
    snprintf(target, sizeof target, "%s/bus/%s", ROOT, name);
    unlink(linkp); assert(symlink(target, linkp) == 0);
}

/* fabricate an event with the given SUBSYSTEM/DEVTYPE/DEVPATH (any may be NULL) */
static void ev_set(struct uevent *ev, const char *sub, const char *dt, const char *dp) {
    ev->n = 0;
    if (sub) { strcpy(ev->key[ev->n], "SUBSYSTEM"); strcpy(ev->val[ev->n], sub); ev->n++; }
    if (dt)  { strcpy(ev->key[ev->n], "DEVTYPE");   strcpy(ev->val[ev->n], dt);  ev->n++; }
    if (dp)  { strcpy(ev->key[ev->n], "DEVPATH");   strcpy(ev->val[ev->n], dp);  ev->n++; }
}

static void test_select(void) {
    struct uevent ev;

    /* usb_device with modalias, parent usb -> HWDB|USB|PATH */
    mkdirs("/devices/pci0/usb1/1-2");
    writef("/devices/pci0/usb1/1-2/modalias", "usb:v1234p5678\n");
    set_subsystem("/devices/pci0", "pci");
    set_subsystem("/devices/pci0/usb1", "usb");
    set_subsystem("/devices/pci0/usb1/1-2", "usb");
    ev_set(&ev, "usb", "usb_device", "/devices/pci0/usb1/1-2");
    int s = ub_select(ROOT, "/devices/pci0/usb1/1-2", "/dev/bus/usb/001/002", &ev);
    assert(s & UB_USB); assert(s & UB_HWDB); assert(s & UB_PATH);
    assert(!(s & UB_BLKID)); assert(!(s & UB_NET)); assert(!(s & UB_INPUT));

    /* virtual block disk (zram0): BLKID only, PATH suppressed by /virtual/ + no bus ancestor */
    mkdirs("/devices/virtual/block/zram0");
    set_subsystem("/devices/virtual/block/zram0", "block");
    ev_set(&ev, "block", "disk", "/devices/virtual/block/zram0");
    s = ub_select(ROOT, "/devices/virtual/block/zram0", "/dev/zram0", &ev);
    assert(s & UB_BLKID); assert(!(s & UB_PATH));

    /* real disk (nvme): BLKID + PATH */
    mkdirs("/devices/pci0/nvme/nvme0/nvme0n1");
    set_subsystem("/devices/pci0/nvme/nvme0/nvme0n1", "block");
    ev_set(&ev, "block", "disk", "/devices/pci0/nvme/nvme0/nvme0n1");
    s = ub_select(ROOT, "/devices/pci0/nvme/nvme0/nvme0n1", "/dev/nvme0n1", &ev);
    assert(s & UB_BLKID); assert(s & UB_PATH);

    /* sr0 optical: BLKID suppressed by KERNEL sr*, CDROM fires */
    mkdirs("/devices/pci0/ata/sr0");
    set_subsystem("/devices/pci0/ata/sr0", "block");
    ev_set(&ev, "block", "disk", "/devices/pci0/ata/sr0");
    s = ub_select(ROOT, "/devices/pci0/ata/sr0", "/dev/sr0", &ev);
    assert(!(s & UB_BLKID)); assert(s & UB_CDROM);

    /* mmcblk0boot0: BLKID suppressed */
    mkdirs("/devices/mmc/mmcblk0boot0");
    set_subsystem("/devices/mmc/mmcblk0boot0", "block");
    ev_set(&ev, "block", "disk", "/devices/mmc/mmcblk0boot0");
    s = ub_select(ROOT, "/devices/mmc/mmcblk0boot0", "/dev/mmcblk0boot0", &ev);
    assert(!(s & UB_BLKID));

    /* net device: NET only (no modalias here) */
    mkdirs("/devices/pci0/net/eth0");
    set_subsystem("/devices/pci0/net/eth0", "net");
    ev_set(&ev, "net", NULL, "/devices/pci0/net/eth0");
    s = ub_select(ROOT, "/devices/pci0/net/eth0", NULL, &ev);
    assert(s & UB_NET); assert(!(s & UB_BLKID)); assert(!(s & UB_USB));

    /* input device: INPUT (+PATH via pci ancestor) */
    mkdirs("/devices/pci0/input/input5");
    set_subsystem("/devices/pci0/input/input5", "input");
    ev_set(&ev, "input", NULL, "/devices/pci0/input/input5");
    s = ub_select(ROOT, "/devices/pci0/input/input5", NULL, &ev);
    assert(s & UB_INPUT);

    /* usb interface (DEVTYPE=usb_interface, NOT usb_device): USB suppressed */
    mkdirs("/devices/pci0/usb1/1-2/1-2:1.0");
    set_subsystem("/devices/pci0/usb1/1-2/1-2:1.0", "usb");
    ev_set(&ev, "usb", "usb_interface", "/devices/pci0/usb1/1-2/1-2:1.0");
    s = ub_select(ROOT, "/devices/pci0/usb1/1-2/1-2:1.0", NULL, &ev);
    assert(!(s & UB_USB));

    printf("test_udev_builtins ub_select: OK\n");
}

/* dissect_image dispatches by device type: on a partition uevent carrying an
   inherited designator list, run_builtin_bit(UB_DISSECT) runs copy and sets
   ID_DISSECT_PART_DESIGNATOR. */
static void test_dissect_dispatch(void) {
    struct uevent ev; ev.n = 0;
    bpt_emit(&ev, "DEVTYPE", "partition");
    bpt_emit(&ev, "ID_PART_ENTRY_NUMBER", "1");
    bpt_emit(&ev, "ID_DISSECT_PART1_DESIGNATOR", "esp");
    int rc = run_builtin_bit("", "/devices/x/block/nvme0n1/nvme0n1p1", "/dev/nvme0n1p1", &ev, UB_DISSECT);
    assert(rc >= 0);
    assert(!strcmp(uevent_get(&ev, "ID_DISSECT_PART_DESIGNATOR"), "esp"));
    printf("test_dissect_dispatch OK\n");
}

int main(void) {
    root_make();
    assert(strcmp(ub_kernel_name("/devices/pci0000:00/0000:00:01.0/net/enp6s0"), "enp6s0") == 0);
    assert(strcmp(ub_kernel_name("sda"), "sda") == 0);

    /* ub_in */
    static const char *const set[] = { "pci", "usb", "platform", NULL };
    assert(ub_in("usb", set) == 1);
    assert(ub_in("acpi", set) == 0);
    assert(ub_in(NULL, set) == 0);

    /* ub_has_modalias */
    mkdirs("/devices/dev_ma");
    writef("/devices/dev_ma/modalias", "pci:v00001022d\n");
    assert(ub_has_modalias(ROOT, "/devices/dev_ma") == 1);
    mkdirs("/devices/dev_noma");
    assert(ub_has_modalias(ROOT, "/devices/dev_noma") == 0);

    /* ub_ancestor_in: leaf 'net', parent 'pci' */
    mkdirs("/devices/pci0000:00/0000:00:01.0/net/eth0");
    set_subsystem("/devices/pci0000:00/0000:00:01.0", "pci");
    set_subsystem("/devices/pci0000:00/0000:00:01.0/net/eth0", "net");
    static const char *const busset[] = { "pci", "usb", "platform", "acpi", NULL };
    assert(ub_ancestor_in(ROOT, "/devices/pci0000:00/0000:00:01.0/net/eth0", busset) == 1);
    /* virtual device: no pci/usb/platform/acpi ancestor */
    mkdirs("/devices/virtual/block/zram0");
    set_subsystem("/devices/virtual/block/zram0", "block");
    assert(ub_ancestor_in(ROOT, "/devices/virtual/block/zram0", busset) == 0);

    printf("test_udev_builtins helpers: OK\n");
    test_select();
    test_dissect_dispatch();
    return 0;
}
