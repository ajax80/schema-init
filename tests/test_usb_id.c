#include "../usb_id.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* --- fake-sysfs builders (same idiom as test_path_id.c) --- */
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

static void test_encoders(void) {
    char out[USB_STR_MAX];

    usb_plain("USB OPTICAL MOUSE ", out, sizeof out);
    assert(strcmp(out, "USB_OPTICAL_MOUSE") == 0);

    usb_plain("GenesysLogic Technology Co., Ltd.", out, sizeof out);
    assert(strcmp(out, "GenesysLogic_Technology_Co.__Ltd.") == 0);

    usb_plain("Expansion       ", out, sizeof out);
    assert(strcmp(out, "Expansion") == 0);

    usb_encode("GenesysLogic Technology Co., Ltd.", out, sizeof out);
    assert(strcmp(out, "GenesysLogic\\x20Technology\\x20Co.\\x2c\\x20Ltd.") == 0);

    usb_encode("Seagate ", out, sizeof out);
    assert(strcmp(out, "Seagate\\x20") == 0);

    usb_encode("Expansion       ", out, sizeof out);
    assert(strcmp(out, "Expansion\\x20\\x20\\x20\\x20\\x20\\x20\\x20") == 0);

    /* SAFE chars survive both forms */
    usb_plain("7.0.12-cachyos1 x86_64", out, sizeof out);
    assert(strcmp(out, "7.0.12-cachyos1_x86_64") == 0);
    usb_encode("a:b-c.d", out, sizeof out);
    assert(strcmp(out, "a:b-c.d") == 0);

    printf("test_encoders OK\n");
}

/* Build a minimal usb_device + interface + a leaf under the interface.
 * Returns via out_leaf the relative devpath of the leaf. */
static void build_usb_dev(const char *root, const char *pci_rel,
                          const char *devname,     /* e.g. "1-4" */
                          const char *ifname,       /* e.g. "1-4:1.0" */
                          const char *manufacturer, /* NULL to omit */
                          const char *product,      /* NULL to omit */
                          const char *serial,       /* NULL to omit */
                          const char *vid, const char *pid, const char *bcd,
                          char *out_leaf, size_t leafsz) {
    char utgt[2100], dir[2600], sub[2700], f[2800];
    if ((size_t)snprintf(utgt, sizeof utgt, "%s/bus/usb", root) >= sizeof utgt) assert(0);
    mkdirp(utgt);

    /* usb_device dir */
    char devrel[1024];
    if ((size_t)snprintf(devrel, sizeof devrel, "%s/%s", pci_rel, devname) >= sizeof devrel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, devrel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, utgt);
    if ((size_t)snprintf(f, sizeof f, "%s/idVendor", dir) >= sizeof f) assert(0);
    mkfile(f, vid);
    if ((size_t)snprintf(f, sizeof f, "%s/idProduct", dir) >= sizeof f) assert(0);
    mkfile(f, pid);
    if ((size_t)snprintf(f, sizeof f, "%s/bcdDevice", dir) >= sizeof f) assert(0);
    mkfile(f, bcd);
    if (manufacturer) {
        if ((size_t)snprintf(f, sizeof f, "%s/manufacturer", dir) >= sizeof f) assert(0);
        mkfile(f, manufacturer);
    }
    if (product) {
        if ((size_t)snprintf(f, sizeof f, "%s/product", dir) >= sizeof f) assert(0);
        mkfile(f, product);
    }
    if (serial) {
        if ((size_t)snprintf(f, sizeof f, "%s/serial", dir) >= sizeof f) assert(0);
        mkfile(f, serial);
    }

    /* usb_interface dir */
    char ifrel[1200];
    if ((size_t)snprintf(ifrel, sizeof ifrel, "%s/%s", devrel, ifname) >= sizeof ifrel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, ifrel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, utgt);
    if ((size_t)snprintf(f, sizeof f, "%s/bInterfaceNumber", dir) >= sizeof f) assert(0);
    mkfile(f, "00\n");
    if ((size_t)snprintf(f, sizeof f, "%s/bInterfaceClass", dir) >= sizeof f) assert(0);
    mkfile(f, "0e\n");
    if ((size_t)snprintf(f, sizeof f, "%s/bInterfaceSubClass", dir) >= sizeof f) assert(0);
    mkfile(f, "01\n");
    if ((size_t)snprintf(f, sizeof f, "%s/bInterfaceProtocol", dir) >= sizeof f) assert(0);
    mkfile(f, "00\n");

    /* leaf under the interface (subsystem=video4linux) */
    char vtgt[2100];
    if ((size_t)snprintf(vtgt, sizeof vtgt, "%s/class/video4linux", root) >= sizeof vtgt) assert(0);
    mkdirp(vtgt);
    char leafrel[1400];
    if ((size_t)snprintf(leafrel, sizeof leafrel, "%s/video4linux/video0", ifrel) >= sizeof leafrel) assert(0);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, leafrel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(sub, sizeof sub, "%s/subsystem", dir) >= sizeof sub) assert(0);
    mklink(sub, vtgt);

    safe_copy(out_leaf, leafrel, leafsz);
}

static void test_discovery_and_names(void) {
    char tmpl[] = "/tmp/schema-usbid-d-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    char leaf[2048];
    build_usb_dev(root, "/devices/pci0000:00/0000:02:00.0/usb1", "1-4", "1-4:1.0",
                  "GenesysLogic Technology Co., Ltd.", "USB2.0 UVC PC Camera", NULL,
                  "a16f\n", "0304\n", "0620\n", leaf, sizeof leaf);

    char devdir[PATH_MAX], ifdir[PATH_MAX];
    assert(usb_find_nodes(root, leaf, devdir, sizeof devdir, ifdir, sizeof ifdir) == 0);
    assert(strcmp(pi_base(devdir), "1-4") == 0);
    assert(strcmp(pi_base(ifdir), "1-4:1.0") == 0);

    char plain[USB_STR_MAX], enc[USB_STR_MAX];
    usb_name_field(devdir, "manufacturer", "a16f", plain, sizeof plain, enc, sizeof enc);
    assert(strcmp(plain, "GenesysLogic_Technology_Co.__Ltd.") == 0);
    assert(strcmp(enc, "GenesysLogic\\x20Technology\\x20Co.\\x2c\\x20Ltd.") == 0);

    /* fallback: no manufacturer -> both forms = idVendor hex */
    char tmpl2[] = "/tmp/schema-usbid-f-XXXXXX";
    char *root2 = mkdtemp(tmpl2);
    assert(root2);
    char leaf2[2048];
    build_usb_dev(root2, "/devices/pci0000:00/0000:02:00.0/usb1", "1-5", "1-5:1.0",
                  NULL, "USB OPTICAL MOUSE ", NULL, "18f8\n", "0f99\n", "0100\n",
                  leaf2, sizeof leaf2);
    char devdir2[PATH_MAX], ifdir2[PATH_MAX];
    assert(usb_find_nodes(root2, leaf2, devdir2, sizeof devdir2, ifdir2, sizeof ifdir2) == 0);
    usb_name_field(devdir2, "manufacturer", "18f8", plain, sizeof plain, enc, sizeof enc);
    assert(strcmp(plain, "18f8") == 0);
    assert(strcmp(enc, "18f8") == 0);
    usb_name_field(devdir2, "product", "0f99", plain, sizeof plain, enc, sizeof enc);
    assert(strcmp(plain, "USB_OPTICAL_MOUSE") == 0);

    printf("test_discovery_and_names OK\n");
}

static void mk_iface(const char *root, const char *ifrel,
                     const char *num, const char *cls, const char *sub, const char *pro,
                     const char *driver) {
    char utgt[2100], dir[2600], link[2700], f[2800];
    if ((size_t)snprintf(utgt, sizeof utgt, "%s/bus/usb", root) >= sizeof utgt) assert(0);
    mkdirp(utgt);
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", root, ifrel) >= sizeof dir) assert(0);
    mkdirp(dir);
    if ((size_t)snprintf(link, sizeof link, "%s/subsystem", dir) >= sizeof link) assert(0);
    mklink(link, utgt);
    if ((size_t)snprintf(f, sizeof f, "%s/bInterfaceNumber", dir) >= sizeof f) assert(0);
    mkfile(f, num);
    if ((size_t)snprintf(f, sizeof f, "%s/bInterfaceClass", dir) >= sizeof f) assert(0);
    mkfile(f, cls);
    if ((size_t)snprintf(f, sizeof f, "%s/bInterfaceSubClass", dir) >= sizeof f) assert(0);
    mkfile(f, sub);
    if ((size_t)snprintf(f, sizeof f, "%s/bInterfaceProtocol", dir) >= sizeof f) assert(0);
    mkfile(f, pro);
    if (driver) {
        char dtgt[2200];
        if ((size_t)snprintf(dtgt, sizeof dtgt, "%s/bus/usb/drivers/%s", root, driver) >= sizeof dtgt) assert(0);
        mkdirp(dtgt);
        if ((size_t)snprintf(link, sizeof link, "%s/driver", dir) >= sizeof link) assert(0);
        mklink(link, dtgt);
    }
}

static void test_type_driver_interfaces(void) {
    char tmpl[] = "/tmp/schema-usbid-t-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    const char *devrel = "/devices/pci0000:00/0000:02:00.0/usb1/1-7";
    char devdir[PATH_MAX];
    if ((size_t)snprintf(devdir, sizeof devdir, "%s%s", root, devrel) >= sizeof devdir) assert(0);
    mkdirp(devdir);

    /* pico key: ifaces 0,1,2,3 = 030000, 030000(dup), 0b0000, ff0000 */
    char ifrel[1200];
    if ((size_t)snprintf(ifrel, sizeof ifrel, "%s/1-7:1.0", devrel) >= sizeof ifrel) assert(0);
    mk_iface(root, ifrel, "00\n","03\n","00\n","00\n","usbhid");
    if ((size_t)snprintf(ifrel, sizeof ifrel, "%s/1-7:1.1", devrel) >= sizeof ifrel) assert(0);
    mk_iface(root, ifrel, "01\n","03\n","00\n","00\n",NULL);
    if ((size_t)snprintf(ifrel, sizeof ifrel, "%s/1-7:1.2", devrel) >= sizeof ifrel) assert(0);
    mk_iface(root, ifrel, "02\n","0b\n","00\n","00\n",NULL);
    if ((size_t)snprintf(ifrel, sizeof ifrel, "%s/1-7:1.3", devrel) >= sizeof ifrel) assert(0);
    mk_iface(root, ifrel, "03\n","ff\n","00\n","00\n",NULL);

    char out[USB_STR_MAX];
    usb_interfaces(devdir, out, sizeof out);
    assert(strcmp(out, ":030000:0b0000:ff0000:") == 0);   /* number order + dedup */

    /* type from the class-0e interface */
    char ifdir[PATH_MAX];
    if ((size_t)snprintf(ifdir, sizeof ifdir, "%s%s/1-7:1.0", root, devrel) >= sizeof ifdir) assert(0);
    assert(strcmp(usb_type_from_iface(ifdir), "hid") == 0);

    char drv[64];
    assert(usb_driver(ifdir, drv, sizeof drv) == 0);
    assert(strcmp(drv, "usbhid") == 0);

    /* mouse ordering (not value-sorted): ifaces 0=030102, 1=030101 */
    char tmpl2[] = "/tmp/schema-usbid-m-XXXXXX";
    char *root2 = mkdtemp(tmpl2);
    assert(root2);
    const char *drel2 = "/devices/pci0000:00/0000:02:00.0/usb1/1-5";
    char dd2[PATH_MAX];
    if ((size_t)snprintf(dd2, sizeof dd2, "%s%s", root2, drel2) >= sizeof dd2) assert(0);
    mkdirp(dd2);
    if ((size_t)snprintf(ifrel, sizeof ifrel, "%s/1-5:1.0", drel2) >= sizeof ifrel) assert(0);
    mk_iface(root2, ifrel, "00\n","03\n","01\n","02\n",NULL);
    if ((size_t)snprintf(ifrel, sizeof ifrel, "%s/1-5:1.1", drel2) >= sizeof ifrel) assert(0);
    mk_iface(root2, ifrel, "01\n","03\n","01\n","01\n",NULL);
    usb_interfaces(dd2, out, sizeof out);
    assert(strcmp(out, ":030102:030101:") == 0);

    /* type map: mass storage class 08 subclass 06 -> disk */
    char tmpl3[] = "/tmp/schema-usbid-s-XXXXXX";
    char *root3 = mkdtemp(tmpl3);
    assert(root3);
    const char *sifrel = "/devices/pci0000:00/x/4-1/4-1:1.0";
    mk_iface(root3, sifrel, "00\n","08\n","06\n","50\n","usb-storage");
    char sifdir[PATH_MAX];
    if ((size_t)snprintf(sifdir, sizeof sifdir, "%s%s", root3, sifrel) >= sizeof sifdir) assert(0);
    assert(strcmp(usb_type_from_iface(sifdir), "disk") == 0);

    printf("test_type_driver_interfaces OK\n");
}

static const char *ev_get(const struct uevent *ev, const char *k) {
    for (int i = 0; i < ev->n; i++) if (strcmp(ev->key[i], k) == 0) return ev->val[i];
    return NULL;
}

static void test_build_full(void) {
    char tmpl[] = "/tmp/schema-usbid-b-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    char leaf[2048];
    /* camera: manufacturer + product, NO serial; interface 1-4:1.0 class 0e (video) */
    build_usb_dev(root, "/devices/pci0000:00/0000:02:00.0/usb1", "1-4", "1-4:1.0",
                  "GenesysLogic Technology Co., Ltd.", "USB2.0 UVC PC Camera", NULL,
                  "a16f\n", "0304\n", "0620\n", leaf, sizeof leaf);
    /* add a second interface so ID_USB_INTERFACES has two entries like the real camera.
     * build_usb_dev's interface is class 0e sub 01 -> 0e0100; this second is 0e0200 */
    mk_iface(root, "/devices/pci0000:00/0000:02:00.0/usb1/1-4/1-4:1.1",
             "01\n", "0e\n", "02\n", "00\n", NULL);
    /* set the interface driver so ID_USB_DRIVER is present */
    {
        char dtgt[2200], link[2600];
        if ((size_t)snprintf(dtgt, sizeof dtgt, "%s/bus/usb/drivers/uvcvideo", root) >= sizeof dtgt) assert(0);
        mkdirp(dtgt);
        if ((size_t)snprintf(link, sizeof link, "%s/devices/pci0000:00/0000:02:00.0/usb1/1-4/1-4:1.0/driver", root) >= sizeof link) assert(0);
        mklink(link, dtgt);
    }

    struct uevent ev;
    assert(usb_id_build(root, leaf, &ev) == 0);

    assert(strcmp(ev_get(&ev, "ID_BUS"), "usb") == 0);
    assert(strcmp(ev_get(&ev, "ID_VENDOR_ID"), "a16f") == 0);
    assert(strcmp(ev_get(&ev, "ID_MODEL_ID"), "0304") == 0);
    assert(strcmp(ev_get(&ev, "ID_REVISION"), "0620") == 0);
    assert(strcmp(ev_get(&ev, "ID_VENDOR"), "GenesysLogic_Technology_Co.__Ltd.") == 0);
    assert(strcmp(ev_get(&ev, "ID_VENDOR_ENC"), "GenesysLogic\\x20Technology\\x20Co.\\x2c\\x20Ltd.") == 0);
    assert(strcmp(ev_get(&ev, "ID_MODEL"), "USB2.0_UVC_PC_Camera") == 0);
    assert(strcmp(ev_get(&ev, "ID_MODEL_ENC"), "USB2.0\\x20UVC\\x20PC\\x20Camera") == 0);
    assert(strcmp(ev_get(&ev, "ID_SERIAL"),
                  "GenesysLogic_Technology_Co.__Ltd._USB2.0_UVC_PC_Camera") == 0);
    assert(ev_get(&ev, "ID_SERIAL_SHORT") == NULL);          /* no serial sysattr */
    assert(strcmp(ev_get(&ev, "ID_TYPE"), "video") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_MODEL"), "USB2.0_UVC_PC_Camera") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_TYPE"), "video") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_INTERFACES"), ":0e0100:0e0200:") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_INTERFACE_NUM"), "00") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_DRIVER"), "uvcvideo") == 0);

    /* with a serial: composition includes _SHORT */
    char tmpl2[] = "/tmp/schema-usbid-b2-XXXXXX";
    char *root2 = mkdtemp(tmpl2);
    assert(root2);
    char leaf2[2048];
    build_usb_dev(root2, "/devices/pci0000:00/0000:02:00.0/usb1", "1-7", "1-7:1.0",
                  "Pol Henarejos", "Pico Key", "44BA59F930300000",
                  "2e8a\n", "10fe\n", "0806\n", leaf2, sizeof leaf2);
    struct uevent ev2;
    assert(usb_id_build(root2, leaf2, &ev2) == 0);
    assert(strcmp(ev_get(&ev2, "ID_SERIAL"), "Pol_Henarejos_Pico_Key_44BA59F930300000") == 0);
    assert(strcmp(ev_get(&ev2, "ID_SERIAL_SHORT"), "44BA59F930300000") == 0);
    assert(strcmp(ev_get(&ev2, "ID_USB_SERIAL_SHORT"), "44BA59F930300000") == 0);

    printf("test_build_full OK\n");
}

int main(void) {
    test_encoders();
    test_discovery_and_names();
    test_type_driver_interfaces();
    test_build_full();
    printf("ALL usb_id tests passed\n");
    return 0;
}
