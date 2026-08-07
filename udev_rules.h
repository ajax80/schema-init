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
           strcmp(key, "ID_INSTANCE") == 0;
}

/* Walk ancestors nearest-first; for each real ancestor device, compute its
 * builtin properties and inherit the bounded ID_* keys the child lacks. */
static inline int rules_import_parent(const char *sysroot, const char *devpath,
                                      struct uevent *ev) {
    int before = ev->n;
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
        for (int i = 0; i < anc.n; i++)
            if (rules_inheritable(anc.key[i]))
                ub_add(ev, anc.key[i], anc.val[i]);   /* first-writer-wins */
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
    rules_composite_hwdb(sysroot, devpath, ev);
    rules_import_parent(sysroot, devpath, ev);
    return ev->n - before;
}

#endif /* UDEV_RULES_H */
