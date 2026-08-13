#include "../path_id.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* --- fake-sysfs builders (same idiom as test_coldplug.c) --- */
static void mkdirp(const char *path) {
    char cmd[8192];
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

    /* trailing whitespace trimmed (like real udev sysattr), internal spaces kept */
    char attrm[1200]; snprintf(attrm, sizeof attrm, "%s/model", devdir);
    mkfile(attrm, "XPG GAMMIX S11 Pro          \n");
    assert(pi_sysattr(devdir, "model", out, sizeof out) == 0);
    assert(strcmp(out, "XPG GAMMIX S11 Pro") == 0);

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

static void mk_pci_node(const char *root, const char *relpath) {
    char dir[4096], sub[4096], tgt[4096];
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, relpath) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    if ((size_t)snprintf(tgt, sizeof tgt, "%s/bus/pci", root) >= sizeof tgt) assert(0);
    mklink(sub, tgt);
}

static void test_pci_bare(void) {
    char tmpl[] = "/tmp/schema-pathid-pci-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* GPU: /devices/pci0000:00/0000:00:03.1/0000:07:00.0  (bridge skipped) */
    char rootdir[2048];
    snprintf(rootdir, sizeof rootdir, "%s/devices/pci0000:00", root);
    mkdirp(rootdir);                                          /* host bridge root: no subsystem */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:03.1");    /* bridge (pci) */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:03.1/0000:07:00.0");

    char out[PATH_ID_MAX], tag[PATH_ID_MAX];
    assert(path_id_build(root, "/devices/pci0000:00/0000:00:03.1/0000:07:00.0",
                         out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:07:00.0") == 0);   /* leaf-most only, bridge skipped */
    assert(path_id_tag(out, tag, sizeof tag) == 0);
    assert(strcmp(tag, "pci-0000_07_00_0") == 0);
    printf("test_pci_bare OK\n");
}

static void test_platform(void) {
    char tmpl[] = "/tmp/schema-pathid-plat-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* bare platform: /devices/platform/AMDI0030:00 */
    char dir[4096], sub[4096], tgt[4096];
    if ((size_t)snprintf(dir, sizeof dir, "%s/devices/platform/AMDI0030:00", root) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(tgt, sizeof tgt, "%s/bus/platform", root) >= sizeof tgt) assert(0);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, tgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, "/devices/platform/AMDI0030:00", out, sizeof out) > 0);
    assert(strcmp(out, "platform-AMDI0030:00") == 0);
    printf("test_platform OK\n");
}

static void test_pci_platform(void) {
    char tmpl[] = "/tmp/schema-pathid-pp-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* pcspkr: /devices/pci0000:00/0000:00:14.3/PNP0800:00 */
    char base[4096];
    if ((size_t)snprintf(base, sizeof base, "%s/devices/pci0000:00", root) >= sizeof base) assert(0);
    mkdirp(base);
    mk_pci_node(root, "/devices/pci0000:00/0000:00:14.3");
    char dir[4096], sub[4096], tgt[4096];
    if ((size_t)snprintf(dir, sizeof dir, "%s/devices/pci0000:00/0000:00:14.3/PNP0800:00", root) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(tgt, sizeof tgt, "%s/bus/platform", root) >= sizeof tgt) assert(0);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, tgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, "/devices/pci0000:00/0000:00:14.3/PNP0800:00",
                         out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:00:14.3-platform-PNP0800:00") == 0);
    printf("test_pci_platform OK\n");
}

static void test_unanchored(void) {
    char tmpl[] = "/tmp/schema-pathid-un-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* a device with only a 'virtual' subsystem chain -> no pci/platform anchor */
    char dir[4096], sub[4096], tgt[4096];
    if ((size_t)snprintf(dir, sizeof dir, "%s/devices/virtual/misc/foo", root) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(tgt, sizeof tgt, "%s/class/misc", root) >= sizeof tgt) assert(0);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, tgt);
    char out[PATH_ID_MAX];
    assert(path_id_build(root, "/devices/virtual/misc/foo", out, sizeof out) == -1);
    printf("test_unanchored OK\n");
}

static void test_usb(void) {
    char tmpl[] = "/tmp/schema-pathid-usb-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* /devices/pci0000:00/0000:00:07.0/0000:02:00.0/usb1/1-4/1-4:1.0/ttyUSB0
       ID_PATH for the tty leaf = pci-0000:02:00.0-usb-0:4:1.0 */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:07.0");
    mk_pci_node(root, "/devices/pci0000:00/0000:00:07.0/0000:02:00.0");

    const char *pcirel = "/devices/pci0000:00/0000:00:07.0/0000:02:00.0";
    /* usb host + device + interface, each subsystem=usb */
    char rel[2048], dir[2600], sub[2600], utgt[2100];
    if ((size_t)snprintf(utgt, sizeof utgt, "%s/bus/usb", root) >= sizeof utgt) assert(0);
    mkdirp(utgt);

    if ((size_t)snprintf(rel, sizeof rel, "%s/usb1", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, utgt);

    if ((size_t)snprintf(rel, sizeof rel, "%s/usb1/1-4", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, utgt);

    if ((size_t)snprintf(rel, sizeof rel, "%s/usb1/1-4/1-4:1.0", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, utgt);

    /* tty leaf */
    char ttytgt[2100];
    if ((size_t)snprintf(ttytgt, sizeof ttytgt, "%s/class/tty", root) >= sizeof ttytgt) assert(0);
    mkdirp(ttytgt);
    if ((size_t)snprintf(rel, sizeof rel, "%s/usb1/1-4/1-4:1.0/ttyUSB0", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, ttytgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, rel, out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:02:00.0-usb-0:4:1.0") == 0);
    printf("test_usb OK\n");
}

static void test_nvme(void) {
    char tmpl[] = "/tmp/schema-pathid-nvme-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* /devices/pci0000:00/0000:00:01.1/0000:01:00.0/nvme/nvme0/nvme0n1
       ID_PATH = pci-0000:01:00.0-nvme-1 */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:01.1");
    mk_pci_node(root, "/devices/pci0000:00/0000:00:01.1/0000:01:00.0");

    const char *pcirel = "/devices/pci0000:00/0000:00:01.1/0000:01:00.0";
    char rel[2048], dir[2600], sub[2600], ntgt[2100], btgt[2100];
    if ((size_t)snprintf(ntgt, sizeof ntgt, "%s/class/nvme", root) >= sizeof ntgt) assert(0);
    mkdirp(ntgt);
    if ((size_t)snprintf(btgt, sizeof btgt, "%s/class/block", root) >= sizeof btgt) assert(0);
    mkdirp(btgt);

    /* container 'nvme' dir: no subsystem */
    if ((size_t)snprintf(dir, sizeof dir, "%s%s/nvme", root, pcirel) >= sizeof dir) assert(0);
    mkdirp(dir);
    /* nvme0 controller: subsystem=nvme */
    if ((size_t)snprintf(rel, sizeof rel, "%s/nvme/nvme0", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, ntgt);
    /* nvme0n1 namespace/block leaf: subsystem=block, nsid=1 */
    if ((size_t)snprintf(rel, sizeof rel, "%s/nvme/nvme0/nvme0n1", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, btgt);
    char nsid[2600];
    if ((size_t)snprintf(nsid, sizeof nsid, "%s/nsid", dir) >= sizeof nsid) assert(0);
    mkfile(nsid, "1\n");

    char out[PATH_ID_MAX];
    assert(path_id_build(root, rel, out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:01:00.0-nvme-1") == 0);
    printf("test_nvme OK\n");
}

static void test_ata(void) {
    char tmpl[] = "/tmp/schema-pathid-ata-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* /devices/pci0000:00/0000:00:01.3/0000:02:00.1/ata1/host0/target0:0:0/0:0:0:0/block/sda
       ID_PATH for the block leaf = pci-0000:02:00.1-ata-1.0 */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:01.3");
    mk_pci_node(root, "/devices/pci0000:00/0000:00:01.3/0000:02:00.1");

    const char *pcirel = "/devices/pci0000:00/0000:00:01.3/0000:02:00.1";
    char rel[2048], dir[2600], sub[2600], stgt[2100], btgt[2100], f[2700];
    if ((size_t)snprintf(stgt, sizeof stgt, "%s/bus/scsi", root) >= sizeof stgt) assert(0);
    mkdirp(stgt);
    if ((size_t)snprintf(btgt, sizeof btgt, "%s/class/block", root) >= sizeof btgt) assert(0);
    mkdirp(btgt);

    /* ata1 (ata_port host); port_no lives at ata1/ata_port/ata1/port_no */
    if ((size_t)snprintf(dir, sizeof dir, "%s%s/ata1", root, pcirel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(f, sizeof f, "%s%s/ata1/ata_port/ata1/port_no", root, pcirel) >= sizeof f) assert(0);
    mkfile(f, "1\n");
    /* ata_device dev1.0 under link1 */
    if ((size_t)snprintf(f, sizeof f, "%s%s/ata1/link1/dev1.0/uevent", root, pcirel) >= sizeof f) assert(0);
    mkfile(f, "DEVTYPE=ata_device\n");
    /* host0/target0:0:0/0:0:0:0 (scsi_device) */
    if ((size_t)snprintf(dir, sizeof dir, "%s%s/ata1/host0", root, pcirel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(rel, sizeof rel, "%s/ata1/host0/target0:0:0/0:0:0:0", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, stgt);
    /* block/sda leaf */
    if ((size_t)snprintf(rel, sizeof rel, "%s/ata1/host0/target0:0:0/0:0:0:0/block/sda", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, btgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, rel, out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:02:00.1-ata-1.0") == 0);
    printf("test_ata OK\n");
}

static void test_usb_scsi_rebase(void) {
    char tmpl[] = "/tmp/schema-pathid-us-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* .../0000:08:00.3/usb4/4-1/4-1:1.0/host9/target9:0:0/9:0:0:0/block/sdd
       ID_PATH = pci-0000:08:00.3-usb-0:1:1.0-scsi-0:0:0:0  (host9 rebased -> 0) */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:07.1");
    mk_pci_node(root, "/devices/pci0000:00/0000:00:07.1/0000:08:00.3");

    const char *pcirel = "/devices/pci0000:00/0000:00:07.1/0000:08:00.3";
    char rel[2048], dir[2600], sub[2600], utgt[2100], stgt[2100], btgt[2100];
    if ((size_t)snprintf(utgt, sizeof utgt, "%s/bus/usb", root) >= sizeof utgt) assert(0);
    mkdirp(utgt);
    if ((size_t)snprintf(stgt, sizeof stgt, "%s/bus/scsi", root) >= sizeof stgt) assert(0);
    mkdirp(stgt);
    if ((size_t)snprintf(btgt, sizeof btgt, "%s/class/block", root) >= sizeof btgt) assert(0);
    mkdirp(btgt);

    const char *usbdirs[] = { "usb4", "usb4/4-1", "usb4/4-1/4-1:1.0" };
    for (int i = 0; i < 3; i++) {
        if ((size_t)snprintf(rel, sizeof rel, "%s/%s", pcirel, usbdirs[i]) >= sizeof rel) assert(0);
        if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
        mkdirp(dir);
        if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
        mklink(sub, utgt);
    }
    /* host9 under the usb interface (only host -> rebases to 0) */
    if ((size_t)snprintf(rel, sizeof rel, "%s/usb4/4-1/4-1:1.0/host9/target9:0:0/9:0:0:0", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, stgt);
    if ((size_t)snprintf(rel, sizeof rel, "%s/usb4/4-1/4-1:1.0/host9/target9:0:0/9:0:0:0/block/sdd", pcirel) >= sizeof rel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, rel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, btgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, rel, out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:08:00.3-usb-0:1:1.0-scsi-0:0:0:0") == 0);
    printf("test_usb_scsi_rebase OK\n");
}

int main(void) {
    test_tag();
    test_helpers();
    test_pci_bare();
    test_platform();
    test_pci_platform();
    test_unanchored();
    test_usb();
    test_nvme();
    test_ata();
    test_usb_scsi_rebase();
    printf("ALL path_id tests passed\n");
    return 0;
}
