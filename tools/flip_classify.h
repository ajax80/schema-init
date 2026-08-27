/* flip_classify.h — eligibility severity for the schema-udev flip.
 *
 * Permissive model: a divergence blocks the flip only when it is genuinely
 * harmful. Extra symlinks/tags are a superset and break nothing; a MISSING
 * symlink is harmful only when it is an exact-path link that boot/fstab/
 * crypttab reference by name, since those cannot fall back to the kernel
 * devnode or a sibling link. Everything else the boot healthcheck backstops. */
#ifndef FLIP_CLASSIFY_H
#define FLIP_CLASSIFY_H
#include <string.h>

/* A missing link of this kind is harmful (fstab/boot resolves devices by these
 * exact paths). by-id is handled separately as known-debt, not here. */
static inline int link_is_critical(const char *link) {
    return strncmp(link, "disk/by-uuid/",      13) == 0
        || strncmp(link, "disk/by-partuuid/",  17) == 0
        || strncmp(link, "disk/by-label/",     14) == 0
        || strncmp(link, "disk/by-partlabel/", 18) == 0;
}

#endif /* FLIP_CLASSIFY_H */
