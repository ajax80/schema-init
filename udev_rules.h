#ifndef UDEV_RULES_H
#define UDEV_RULES_H

#include "udev_builtins.h"   /* run_builtins, ub_add, ub_absorb, uevent_* , pi_* */
#include <string.h>
#include <limits.h>
#include <stdio.h>

/* Keys udev propagates from an ancestor to a child (IMPORT{parent}).
 * Bounded set — confirmed by the Task 2 parity measurement. */
static inline int rules_inheritable(const char *key) {
    return strcmp(key, "ID_PATH") == 0 ||
           strcmp(key, "ID_PATH_TAG") == 0 ||
           strncmp(key, "ID_USB_", 7) == 0 ||
           strstr(key, "_FROM_DATABASE") != NULL ||
           strcmp(key, "ID_VENDOR") == 0 ||
           strcmp(key, "ID_VENDOR_ENC") == 0 ||
           strcmp(key, "ID_VENDOR_ID") == 0 ||
           strcmp(key, "ID_MODEL") == 0 ||
           strcmp(key, "ID_MODEL_ENC") == 0 ||
           strcmp(key, "ID_MODEL_ID") == 0 ||
           strcmp(key, "ID_SERIAL") == 0 ||
           strcmp(key, "ID_SERIAL_SHORT") == 0 ||
           strcmp(key, "ID_REVISION") == 0 ||
           strcmp(key, "ID_TYPE") == 0 ||
           strcmp(key, "ID_BUS") == 0 ||
           strcmp(key, "ID_INSTANCE") == 0 ||
           strncmp(key, "ID_WWN", 6) == 0 ||
           strcmp(key, "ID_ATA") == 0;
}

static inline int udev_identity_key(const char *key) {
    return strncmp(key, "ID_SERIAL", 9) == 0 || strncmp(key, "ID_MODEL", 8) == 0 ||
           strncmp(key, "ID_VENDOR", 9) == 0 || strcmp(key, "ID_REVISION") == 0 ||
           strcmp(key, "ID_BUS") == 0 || strcmp(key, "ID_TYPE") == 0 ||
           strcmp(key, "ID_USB_TYPE") == 0 || strcmp(key, "ID_WWN") == 0 ||
           strcmp(key, "ID_WWN_WITH_EXTENSION") == 0 || strcmp(key, "ID_INSTANCE") == 0;
}

/* On block devices, storage identity + type + usb-descriptor strings come from
 * ata_id/scsi_id/cdrom_id/usb-storage (not yet reimplemented), NOT the usb parent
 * — and udev formats them differently (trailing-space padding, "-0:0" lun suffix).
 * So a block device inherits ONLY topology/db keys plus the two interface-topology
 * keys udev genuinely carries onto usb-storage nodes. Everything else is bypassed. */
static inline int rules_block_bypass(const struct uevent *anc, const char *key) {
    if (strcmp(key, "ID_PATH") == 0 || strcmp(key, "ID_PATH_TAG") == 0 ||
        strstr(key, "_FROM_DATABASE") != NULL ||
        strcmp(key, "ID_USB_INTERFACE_NUM") == 0 || strcmp(key, "ID_USB_DRIVER") == 0)
        return 0;   /* allowed to inherit */
    if ((strcmp(key, "ID_ATA") == 0 || udev_identity_key(key)) &&
        uevent_get(anc, "ID_BUS") &&
        (strcmp(uevent_get(anc, "ID_BUS"), "ata") == 0 ||
         strcmp(uevent_get(anc, "ID_BUS"), "usb") == 0))
        return 0;   /* ATA/usb-storage disk identity allowed onto its partitions */
    return 1;       /* bypass */
}

/* usb_id computes ID_USB_INTERFACE_NUM/ID_USB_DRIVER/ID_USB_TYPE/ID_TYPE on a
 * usb_interface device; children (input/hidraw/sound/video4linux) inherit them.
 * Our usb_id builtin only fires on usb_device, so synthesize the interface keys
 * here (reusing usb_id.h's iftype map + driver reader for exact parity). */
static inline int rules_usb_interface(const char *sysroot, const char *devpath,
                                      struct uevent *ev) {
    char ifdir[PATH_MAX], num[16], drv[64];
    if ((size_t)snprintf(ifdir, sizeof ifdir, "%s%s", sysroot, devpath) >= sizeof ifdir) return 0;
    if (pi_sysattr(ifdir, "bInterfaceNumber", num, sizeof num) != 0) return 0;  /* not an interface */
    int before = ev->n;
    ub_add(ev, "ID_USB_INTERFACE_NUM", num);
    if (usb_driver(ifdir, drv, sizeof drv) == 0) ub_add(ev, "ID_USB_DRIVER", drv);
    const char *type = usb_type_from_iface(ifdir, devpath);
    if (type) { ub_add(ev, "ID_USB_TYPE", type); ub_add(ev, "ID_TYPE", type); }
    return ev->n - before;
}

static inline int rules_import_subsystem_inherits(const char *sub) {
    if (!sub) return 0;
    return strcmp(sub, "input") == 0 || strcmp(sub, "sound") == 0 ||
           strcmp(sub, "video4linux") == 0 || strcmp(sub, "hidraw") == 0 ||
           strcmp(sub, "net") == 0 || strcmp(sub, "tty") == 0 ||
           strcmp(sub, "block") == 0 || strcmp(sub, "usb") == 0 ||
           strcmp(sub, "pci") == 0 || strcmp(sub, "platform") == 0 ||
           strcmp(sub, "drm") == 0 || strcmp(sub, "graphics") == 0 ||
           strcmp(sub, "rfkill") == 0 || strcmp(sub, "leds") == 0 ||
           strcmp(sub, "bluetooth") == 0 || strcmp(sub, "media") == 0 ||
           strcmp(sub, "gpio") == 0;
}

/* Walk ancestors nearest-first; for each real ancestor device, compute its
 * builtin properties and inherit the bounded ID_* keys the child lacks. */
static inline int rules_import_parent(const char *sysroot, const char *devpath,
                                      struct uevent *ev) {
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    if (!rules_import_subsystem_inherits(sub)) return 0;
    int before = ev->n;
    int is_block = (sub && strcmp(sub, "block") == 0);
    char cur[PATH_MAX];
    if ((size_t)snprintf(cur, sizeof cur, "%s", devpath) >= sizeof cur) return 0;
    while (pi_parent(cur) == 0) {
        /* stop once we've climbed above /devices */
        if (strncmp(cur, "/devices", 8) != 0) break;
        char sysdir[PATH_MAX];
        if ((size_t)snprintf(sysdir, sizeof sysdir, "%s%s", sysroot, cur) >= sizeof sysdir) break;
        struct uevent anc;
        if (uevent_from_sysfs(sysroot, sysdir, &anc) != 0) continue;  /* not a device dir */
        const char *devname = uevent_get(&anc, "DEVNAME");
        char devnode[UE_VAL_MAX]; const char *dn = NULL;
        if (devname) { snprintf(devnode, sizeof devnode, "/dev/%s", devname); dn = devnode; }
        run_builtins(sysroot, cur, dn, &anc);
        rules_usb_interface(sysroot, cur, &anc);   /* usb_interface keys usb_id skips */
        for (int i = 0; i < anc.n; i++) {
            if (is_block && rules_block_bypass(&anc, anc.key[i])) continue;
            if (rules_inheritable(anc.key[i]))
                ub_add(ev, anc.key[i], anc.val[i]);   /* first-writer-wins */
        }
    }
    return ev->n - before;
}

/* Build usb:vVVVVpPPPP[dDDDD] from idVendor/idProduct[/bcdDevice], hex upper. */
static inline int rules_usb_modalias(const char *vend, const char *prod,
                                     const char *bcd, char *out, size_t outsz) {
    if (!vend || !prod) return -1;
    int w = snprintf(out, outsz, "usb:v%04Xp%04X",
                     (unsigned)strtoul(vend, NULL, 16), (unsigned)strtoul(prod, NULL, 16));
    if (w < 0 || (size_t)w >= outsz) return -1;
    if (bcd && bcd[0]) {
        int w2 = snprintf(out + w, outsz - w, "d%04X", (unsigned)strtoul(bcd, NULL, 16));
        if (w2 < 0 || (size_t)(w + w2) >= outsz) return -1;
    }
    return 0;
}

/* Build OUI:XXXXXX from the first three octets of a MAC (upper, no separators). */
static inline int rules_oui_key(const char *mac, char *out, size_t outsz) {
    if (!mac) return -1;
    char hex[7]; int h = 0;
    for (const char *p = mac; *p && h < 6; p++)
        if (isxdigit((unsigned char)*p)) hex[h++] = (char)toupper((unsigned char)*p);
    if (h < 6) return -1;
    hex[6] = '\0';
    int w = snprintf(out, outsz, "OUI:%s", hex);
    return (w > 0 && (size_t)w < outsz) ? 0 : -1;
}

/* Query the hwdb trie with an arbitrary constructed key, merge results (first-writer-wins). */
static inline void rules_hwdb_lookup(const char *key, struct uevent *ev) {
    struct hwdb h;
    if (hwdb_open("/etc/udev/hwdb.bin", &h) != 0 &&
        hwdb_open("/usr/lib/udev/hwdb.bin", &h) != 0) return;
    struct uevent tmp; tmp.n = 0;
    hwdb_query(&h, key, &tmp);
    hwdb_close(&h);
    ub_absorb(ev, &tmp);
}

/* Construct the per-class synthetic modalias and merge its hwdb result. */
static inline int rules_composite_hwdb(const char *sysroot, const char *devpath,
                                       struct uevent *ev) {
    int before = ev->n;
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    char devdir[PATH_MAX];
    if ((size_t)snprintf(devdir, sizeof devdir, "%s%s", sysroot, devpath) >= sizeof devdir)
        return 0;
    char a[256], b[256], c[256], key[512];

    if (sub && strcmp(sub, "usb") == 0 &&
        pi_sysattr(devdir, "idVendor", a, sizeof a) == 0 &&
        pi_sysattr(devdir, "idProduct", b, sizeof b) == 0) {
        const char *bcd = (pi_sysattr(devdir, "bcdDevice", c, sizeof c) == 0) ? c : NULL;
        if (rules_usb_modalias(a, b, bcd, key, sizeof key) == 0) rules_hwdb_lookup(key, ev);
    }
    if (sub && strcmp(sub, "net") == 0 &&
        pi_sysattr(devdir, "address", a, sizeof a) == 0 &&
        rules_oui_key(a, key, sizeof key) == 0) {
        rules_hwdb_lookup(key, ev);
    }
    return ev->n - before;
}

static inline int run_rules(const char *sysroot, const char *devpath,
                            const char *devnode, struct uevent *ev) {
    (void)devnode;
    int before = ev->n;
    rules_usb_interface(sysroot, devpath, ev);   /* if the device itself is a usb_interface */
    rules_composite_hwdb(sysroot, devpath, ev);
    rules_import_parent(sysroot, devpath, ev);
    return ev->n - before;
}

#endif /* UDEV_RULES_H */
