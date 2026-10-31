#ifndef NS_H
#define NS_H

#include <stdint.h>

#define PROTECT_SYSTEM_NONE 0
#define PROTECT_SYSTEM_BASE 1   /* /usr /boot /efi read-only */
#define PROTECT_SYSTEM_FULL 2   /* + /etc                    */

/* "0" / "1" -> 0 / 1. Returns 0 on success, -1 on any other value. */
int parse_ns_bool(const char *val, uint8_t *out);

/* "0" / "1" / "full" -> PROTECT_SYSTEM_*. Returns 0 on success, -1 otherwise. */
int parse_protect_system(const char *val, uint8_t *out);

#define MAX_RW_PATHS 8

/* Build the private mount view in the forked child, before caps are dropped.
 * rw paths are made writable again inside the read-only mounts; a missing one
 * is skipped. No-op (0) when no read-only/tmp knob is set. On failure returns
 * -1 with errno set and *step naming the failing operation. */
int apply_mount_ns(uint8_t private_tmp, uint8_t protect_system,
                   uint8_t protect_home, const char *const *rw, int n_rw,
                   const char **step);

#endif
