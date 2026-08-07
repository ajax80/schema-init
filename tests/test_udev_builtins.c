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

int main(void) {
    root_make();

    /* ub_kernel_name */
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
    return 0;
}
