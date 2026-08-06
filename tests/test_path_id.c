#include "../path_id.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* --- fake-sysfs builders (same idiom as test_coldplug.c) --- */
static void mkdirp(const char *path) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", path);
    assert(system(cmd) == 0);
}
static void mkfile(const char *path, const char *content) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirp(dir); }
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}
static void mklink(const char *linkpath, const char *target) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", linkpath);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirp(dir); }
    unlink(linkpath);
    assert(symlink(target, linkpath) == 0);
}

static void test_tag(void) {
    char out[PATH_ID_MAX];
    assert(path_id_tag("pci-0000:00:00.0", out, sizeof out) == 0);
    assert(strcmp(out, "pci-0000_00_00_0") == 0);

    assert(path_id_tag("pci-0000:08:00.3-usb-0:1:1.0-scsi-0:0:0:0", out, sizeof out) == 0);
    assert(strcmp(out, "pci-0000_08_00_3-usb-0_1_1_0-scsi-0_0_0_0") == 0);

    char tiny[4];
    assert(path_id_tag("abcd", tiny, sizeof tiny) == -1);   /* overflow */
    printf("test_tag OK\n");
}

static void test_helpers(void) {
    char tmpl[] = "/tmp/schema-pathid-h-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);

    char devdir[1024];
    snprintf(devdir, sizeof devdir, "%s/devices/pci0000:00/0000:02:00.1/ata1", root);
    mkdirp(devdir);

    char sublink[1200], subtgt[1200];
    snprintf(subtgt, sizeof subtgt, "%s/class/ata_port", root);
    mkdirp(subtgt);
    snprintf(sublink, sizeof sublink, "%s/subsystem", devdir);
    assert(symlink(subtgt, sublink) == 0);

    char attr[1200];
    snprintf(attr, sizeof attr, "%s/port_no", devdir);
    mkfile(attr, "6\n");

    char out[256];
    assert(pi_subsystem(devdir, out, sizeof out) == 0);
    assert(strcmp(out, "ata_port") == 0);

    assert(pi_sysattr(devdir, "port_no", out, sizeof out) == 0);
    assert(strcmp(out, "6") == 0);

    assert(strcmp(pi_base(devdir), "ata1") == 0);

    char cur[1024];
    snprintf(cur, sizeof cur, "%s", devdir);
    assert(pi_parent(cur) == 0);
    assert(strcmp(pi_base(cur), "0000:02:00.1") == 0);

    char path[PATH_ID_MAX] = "";
    pi_prepend(path, sizeof path, "usb-0:1:1.0");
    assert(strcmp(path, "usb-0:1:1.0") == 0);
    pi_prepend(path, sizeof path, "pci-0000:08:00.3");
    assert(strcmp(path, "pci-0000:08:00.3-usb-0:1:1.0") == 0);

    printf("test_helpers OK\n");
}

int main(void) {
    test_tag();
    test_helpers();
    printf("ALL path_id tests passed\n");
    return 0;
}
