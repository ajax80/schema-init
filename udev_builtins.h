#ifndef UDEV_BUILTINS_H
#define UDEV_BUILTINS_H

#include "schema-udev.h"
#include "path_id.h"
#include "usb_id.h"
#include "input_id.h"
#include "net_id.h"
#include "blkid_fs.h"   /* pulls in blkid_pt.h */
#include "hwdb.h"
#include "ata_id.h"
#include "v4l_id.h"
#include "cdrom_id.h"

#include <string.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>

/* kernel kobject name = basename of the devpath */
static inline const char *ub_kernel_name(const char *devpath) {
    const char *b = strrchr(devpath, '/');
    return b ? b + 1 : devpath;
}

/* is s (may be NULL) one of the NULL-terminated set? */
static inline int ub_in(const char *s, const char *const *set) {
    if (!s) return 0;
    for (int i = 0; set[i]; i++) if (strcmp(s, set[i]) == 0) return 1;
    return 0;
}

/* does the device have a non-empty modalias sysattr? (udev MODALIAS!="") */
static inline int ub_has_modalias(const char *sysroot, const char *devpath) {
    char devdir[PATH_MAX], buf[UE_VAL_MAX];
    snprintf(devdir, sizeof devdir, "%s%s", sysroot, devpath);
    return pi_sysattr(devdir, "modalias", buf, sizeof buf) == 0 && buf[0] != '\0';
}

/* is this device's OR any ancestor's subsystem in the set? (udev SUBSYSTEMS==) */
static inline int ub_ancestor_in(const char *sysroot, const char *devpath,
                                 const char *const *subs) {
    char cur[PATH_MAX], sub[64];
    snprintf(cur, sizeof cur, "%s%s", sysroot, devpath);
    for (;;) {
        if (pi_subsystem(cur, sub, sizeof sub) == 0 && ub_in(sub, subs)) return 1;
        if (strcmp(cur, sysroot) == 0) break;
        if (pi_parent(cur) != 0) break;
    }
    return 0;
}

static inline int ub_has_ata_ancestor(const char *devpath) {
    for (const char *p = devpath; (p = strstr(p, "/ata")) != NULL; p += 4)
        if (p[4] >= '0' && p[4] <= '9') return 1;
    return 0;
}

enum { UB_HWDB = 1, UB_PATH = 2, UB_USB = 4, UB_INPUT = 8, UB_NET = 16, UB_BLKID = 32, UB_ATA = 64, UB_V4L = 128, UB_CDROM = 256 };

/* Pure guard logic: which builtins apply to this device? Mirrors the IMPORT{builtin}
 * conditions in systemd's shipped /usr/lib/udev/rules.d. Order of the bits is
 * irrelevant; run_builtins imposes the dispatch order. */
static inline int ub_select(const char *sysroot, const char *devpath,
                            const char *devnode, const struct uevent *ev) {
    (void)devnode;
    const char *subsystem = uevent_get(ev, "SUBSYSTEM");
    const char *devtype   = uevent_get(ev, "DEVTYPE");
    const char *kname     = ub_kernel_name(devpath);
    int sel = 0;

    (void)kname;
    if (ub_has_modalias(sysroot, devpath)) sel |= UB_HWDB;

    /* path_id: udev runs it (per its shipped rules) on a specific set of subsystems —
     * pci/usb/platform (direct), rfkill, block disks (non-virtual), and the leaf
     * subsystems whose devices introduce their own path component (input, hidraw).
     * Other subsystems (sound/net/tty/drm/...) get ID_PATH by INHERITING their bus
     * ancestor's value via the rules engine (IMPORT{parent} = sub-project B), not a
     * fresh path_id run — so we do not fire on them here. path_id_build() still
     * returns >0 only when it anchors. */
    static const char *const path_subs[] = {
        "pci", "usb", "platform", "block", "input", "hidraw", "rfkill", NULL
    };
    if (ub_in(subsystem, path_subs) &&
        !(strcmp(subsystem, "block") == 0 && strstr(devpath, "/virtual/")))
        sel |= UB_PATH;

    if (subsystem && strcmp(subsystem, "usb") == 0 &&
        devtype && strcmp(devtype, "usb_device") == 0) sel |= UB_USB;

    static const char *const usb_anc[] = { "usb", NULL };
    if (subsystem && strcmp(subsystem, "block") == 0 &&
        devtype && strcmp(devtype, "disk") == 0 &&
        ub_ancestor_in(sysroot, devpath, usb_anc))
        sel |= UB_USB;

    if (subsystem && strcmp(subsystem, "input") == 0) sel |= UB_INPUT;

    if (subsystem && strcmp(subsystem, "net") == 0) sel |= UB_NET;

    if (subsystem && strcmp(subsystem, "video4linux") == 0) sel |= UB_V4L;

    if (subsystem && strcmp(subsystem, "block") == 0 &&
        devtype && (strcmp(devtype, "disk") == 0 || strcmp(devtype, "partition") == 0) &&
        fnmatch("sr*", kname, 0) != 0 && fnmatch("mmcblk*boot*", kname, 0) != 0)
        sel |= UB_BLKID;

    if (subsystem && strcmp(subsystem, "block") == 0 &&
        devtype && strcmp(devtype, "disk") == 0 &&
        ub_has_ata_ancestor(devpath))
        sel |= UB_ATA;

    if (subsystem && strcmp(subsystem, "block") == 0 &&
        (fnmatch("sr*", kname, 0) == 0 || fnmatch("scd*", kname, 0) == 0))
        sel |= UB_CDROM;

    return sel;
}

/* Append one key=value into ev if the key is not already present (first writer wins,
 * so the kernel payload is never overwritten). */
static inline void ub_add(struct uevent *ev, const char *key, const char *val) {
    if (ev->n >= UE_MAX_KEYS || uevent_get(ev, key)) return;
    safe_copy(ev->key[ev->n], key, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], val, UE_VAL_MAX);
    ev->n++;
}

/* Merge a builtin's output (in a scratch uevent) into ev. The builtins RESET their
 * out->n at entry (they own their buffer), so each must be run into its own scratch
 * uevent and then absorbed here — otherwise a later builtin wipes earlier output and
 * the base kernel payload. */
static inline void ub_absorb(struct uevent *ev, const struct uevent *tmp) {
    for (int i = 0; i < tmp->n; i++) ub_add(ev, tmp->key[i], tmp->val[i]);
}

/* Dispatch: run each selected builtin in fixed udev precedence order, each into a
 * scratch uevent, absorbing its properties into ev. Returns the number added. */
static inline int run_builtins(const char *sysroot, const char *devpath,
                               const char *devnode, struct uevent *ev) {
    int before = ev->n;
    int sel = ub_select(sysroot, devpath, devnode, ev);
    struct uevent tmp;

    if (sel & UB_HWDB)  { tmp.n = 0; hwdb_build(sysroot, devpath, &tmp); ub_absorb(ev, &tmp); }

    if (sel & UB_PATH) {
        char idpath[PATH_ID_MAX], idtag[PATH_ID_MAX];
        if (path_id_build(sysroot, devpath, idpath, sizeof idpath) > 0) {
            ub_add(ev, "ID_PATH", idpath);
            if (path_id_tag(idpath, idtag, sizeof idtag) == 0) ub_add(ev, "ID_PATH_TAG", idtag);
        }
    }

    if (sel & UB_USB)   { tmp.n = 0; usb_id_build(sysroot, devpath, &tmp);   ub_absorb(ev, &tmp); }
    if (sel & UB_INPUT) { tmp.n = 0; input_id_build(sysroot, devpath, &tmp); ub_absorb(ev, &tmp); }
    if (sel & UB_NET)   { tmp.n = 0; net_id_build(sysroot, devpath, &tmp);   ub_absorb(ev, &tmp); }
    if (sel & UB_V4L)   { tmp.n = 0; v4l_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); }
    if (sel & UB_ATA)   { tmp.n = 0; ata_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); }
    if (sel & UB_BLKID) {
        tmp.n = 0; blkid_pt_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp);
        tmp.n = 0; blkid_fs_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp);
    }
    if (sel & UB_CDROM) { tmp.n = 0; cdrom_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); }
    return ev->n - before;
}

#endif /* UDEV_BUILTINS_H */
