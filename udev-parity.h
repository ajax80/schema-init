#ifndef UDEV_PARITY_H
#define UDEV_PARITY_H

#include "schema-udev.h"
#include "udev_db.h"
#include "udev_rules.h"
#include <string.h>
#include <stdio.h>


/* Attribute an E: key to its TRUE owning builtin (honest — do not narrow to make
 * a gate pass; the in-scope classifier below handles legitimate deferrals). */
static inline const char *parity_builtin_hint(const char *key) {
    if (strstr(key, "_FROM_DATABASE")) return "hwdb";
    if (strncmp(key, "ID_INPUT", 8) == 0) return "input_id";
    /* net_id owns only the ID_NET_NAME_{ONBOARD,SLOT,PATH,MAC} + NAMING keys;
     * ID_NET_DRIVER/ID_NET_LINK_FILE/ID_NET_NAME are net_setup_link (out of scope). */
    if (strncmp(key, "ID_NET_NAME_", 12) == 0 || strncmp(key, "ID_NET_NAMING_", 14) == 0) return "net_id";
    if (strncmp(key, "ID_FS_", 6) == 0 || strncmp(key, "ID_PART_", 8) == 0) return "blkid";
    if (strncmp(key, "ID_PATH", 7) == 0) return "path_id";
    if (strncmp(key, "ID_V4L", 6) == 0 || strncmp(key, "ID_VIDEO", 8) == 0) return "v4l_id";
    if (strncmp(key, "ID_USB", 6) == 0 || strncmp(key, "ID_SERIAL", 9) == 0 ||
        strncmp(key, "ID_MODEL", 8) == 0 || strncmp(key, "ID_VENDOR", 9) == 0 ||
        strcmp(key, "ID_REVISION") == 0 || strcmp(key, "ID_BUS") == 0 ||
        strcmp(key, "ID_TYPE") == 0 || strcmp(key, "ID_INSTANCE") == 0 ||
        strcmp(key, "ID_WWN") == 0) return "usb_id";
    return "";
}

/* Sub-features not yet implemented within an in-scope builtin — documented
 * deferrals (blkid geometry deferred in A; path_id compat variants; hwdb OUI). */
static inline int parity_deferred(const char *key) {
    return strcmp(key, "ID_FS_SIZE") == 0 || strcmp(key, "ID_FS_BLOCKSIZE") == 0 ||
           strcmp(key, "ID_FS_LASTBLOCK") == 0 || strcmp(key, "ID_OUI_FROM_DATABASE") == 0 ||
           strcmp(key, "ID_PATH_WITH_USB_REVISION") == 0 || strcmp(key, "ID_PATH_ATA_COMPAT") == 0;
}

static inline int parity_ata_feature(const char *key) {
    if (strncmp(key, "ID_ATA_", 7) != 0) return 0;   /* ID_ATA (=1) is emitted, not deferred */
    return 1;   /* all ID_ATA_* feature-set keys are documented-deferred in slice 3a */
}

/* Is a udev E: key that WE failed to reproduce a genuine in-scope gap?
 * Device-class aware so it cannot be gamed by declassifying key names. */
static inline int parity_in_scope_missing(const char *key, const char *sub,
                                          const char *devpath) {
    const char *hint = parity_builtin_hint(key);
    if (!hint[0]) return 0;                       /* runtime / other-builtin key */
    if (strcmp(hint, "v4l_id") == 0) return 0;    /* v4l_id not reimplemented */
    if (parity_deferred(key)) return 0;           /* documented deferral */
    if (sub && strcmp(sub, "block") == 0) {
        if (parity_ata_feature(key)) return 0;                 /* slice-3a deferral */
        if (strcmp(key, "ID_PATH") == 0 || strcmp(key, "ID_PATH_TAG") == 0 ||
            strstr(key, "_FROM_DATABASE") != NULL ||
            strncmp(key, "ID_FS_", 6) == 0 || strncmp(key, "ID_PART_", 8) == 0 ||
            strcmp(key, "ID_USB_INTERFACE_NUM") == 0 || strcmp(key, "ID_USB_DRIVER") == 0)
            return 1;
        /* identity keys are ours on the ATA chain (ata_id) and the usb chain
         * (usb_id on usb-storage block, slice 3b); real non-usb SCSI stays
         * deferred (scsi_id, no target). */
        if (udev_identity_key(key) && devpath &&
            (strstr(devpath, "/ata") != NULL || strstr(devpath, "/usb") != NULL))
            return 1;
        return 0;
    }
    if (udev_identity_key(key)) {
        int on_usb = devpath && strstr(devpath, "/usb") != NULL;
        return on_usb ? 1 : 0;                    /* usb_id only on usb chain */
    }
    return 1;
}

struct keycount { char key[UE_KEY_MAX]; int count; };

static inline void keycount_add(struct keycount *tab, int *n, int max, const char *key) {
    int i;
    for (i = 0; i < *n; i++)
        if (strcmp(tab[i].key, key) == 0) { tab[i].count++; return; }
    if (*n < max) {
        safe_copy(tab[*n].key, key, UE_KEY_MAX);
        tab[*n].count = 1;
        (*n)++;
    }
}

static inline void keycount_sort_desc(struct keycount *tab, int n) {
    int i, j;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (tab[j].count > tab[i].count) {
                struct keycount t = tab[i]; tab[i] = tab[j]; tab[j] = t;
            }
}

#endif /* UDEV_PARITY_H */
