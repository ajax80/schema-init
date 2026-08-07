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

static inline int run_rules(const char *sysroot, const char *devpath,
                            const char *devnode, struct uevent *ev) {
    (void)devnode;
    int before = ev->n;
    rules_import_parent(sysroot, devpath, ev);
    return ev->n - before;
}

#endif /* UDEV_RULES_H */
