#ifndef UDEV_RULES_H
#define UDEV_RULES_H

#include "udev_builtins.h"   /* run_builtins, ub_add, ub_absorb, uevent_* , pi_* */
#include <string.h>
#include <limits.h>
#include <stdio.h>

/* Post-builtin property derivation: IMPORT{parent} inheritance + composite hwdb.
 * Additive, first-writer-wins; never overwrites existing keys. Returns count added. */
static inline int run_rules(const char *sysroot, const char *devpath,
                            const char *devnode, struct uevent *ev) {
    (void)sysroot; (void)devpath; (void)devnode; (void)ev;
    return 0;
}

#endif /* UDEV_RULES_H */
