#ifndef UDEV_BUILTINS_H
#define UDEV_BUILTINS_H

#include "schema-udev.h"
#include "path_id.h"
#include "usb_id.h"
#include "input_id.h"
#include "net_id.h"
#include "blkid_fs.h"   /* pulls in blkid_pt.h */
#include "hwdb.h"

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

enum { UB_HWDB = 1, UB_PATH = 2, UB_USB = 4, UB_INPUT = 8, UB_NET = 16, UB_BLKID = 32 };

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

    static const char *const path_subs[] = { "pci", "platform", "block", "net", "input", "drm", "graphics", NULL };
    if (ub_has_modalias(sysroot, devpath)) sel |= UB_HWDB;

    if (subsystem && ub_in(subsystem, path_subs)) {
        if (strcmp(subsystem, "block") == 0 && strstr(devpath, "/virtual/")) {
            /* skip virtual block devices */
        } else {
            sel |= UB_PATH;
        }
    } else if (subsystem && strcmp(subsystem, "sound") == 0 && fnmatch("card*", kname, 0) == 0) {
        sel |= UB_PATH;
    } else if (subsystem && strcmp(subsystem, "usb") == 0 && devtype && strcmp(devtype, "usb_device") == 0) {
        sel |= UB_PATH;
    }

    if (subsystem && strcmp(subsystem, "usb") == 0 &&
        devtype && strcmp(devtype, "usb_device") == 0) sel |= UB_USB;

    if (subsystem && strcmp(subsystem, "input") == 0) sel |= UB_INPUT;

    if (subsystem && strcmp(subsystem, "net") == 0) sel |= UB_NET;

    if (subsystem && strcmp(subsystem, "block") == 0 &&
        devtype && (strcmp(devtype, "disk") == 0 || strcmp(devtype, "partition") == 0) &&
        fnmatch("sr*", kname, 0) != 0 && fnmatch("mmcblk*boot*", kname, 0) != 0)
        sel |= UB_BLKID;

    return sel;
}

/* Dispatch: run each selected builtin in fixed udev precedence order. Each builtin
 * appends its properties into ev. Returns the number of properties added. */
static inline int run_builtins(const char *sysroot, const char *devpath,
                               const char *devnode, struct uevent *ev) {
    int before = ev->n;
    int sel = ub_select(sysroot, devpath, devnode, ev);
    if (sel & UB_HWDB)  hwdb_build(sysroot, devpath, ev);
    if (sel & UB_PATH) {
        char idpath[PATH_ID_MAX], idtag[PATH_ID_MAX];
        if (path_id_build(sysroot, devpath, idpath, sizeof idpath) > 0) {
            if (ev->n < UE_MAX_KEYS) {
                safe_copy(ev->key[ev->n], "ID_PATH", UE_KEY_MAX);
                safe_copy(ev->val[ev->n], idpath, UE_VAL_MAX);
                ev->n++;
            }
            if (path_id_tag(idpath, idtag, sizeof idtag) == 0 && ev->n < UE_MAX_KEYS) {
                safe_copy(ev->key[ev->n], "ID_PATH_TAG", UE_KEY_MAX);
                safe_copy(ev->val[ev->n], idtag, UE_VAL_MAX);
                ev->n++;
            }
        }
    }
    if (sel & UB_USB)   usb_id_build(sysroot, devpath, ev);
    if (sel & UB_INPUT) input_id_build(sysroot, devpath, ev);
    if (sel & UB_NET)   net_id_build(sysroot, devpath, ev);
    if (sel & UB_BLKID) { blkid_pt_build(sysroot, devpath, devnode, ev);
                          blkid_fs_build(sysroot, devpath, devnode, ev); }
    return ev->n - before;
}

#endif /* UDEV_BUILTINS_H */
