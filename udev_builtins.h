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

#endif /* UDEV_BUILTINS_H */
