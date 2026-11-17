#ifndef NS_H
#define NS_H

#include <stdint.h>

#define PROTECT_SYSTEM_NONE 0
#define PROTECT_SYSTEM_BASE 1   /* /usr /boot /efi read-only */
#define PROTECT_SYSTEM_FULL 2   /* + /etc                    */

/* kernel-surface knobs, the service_t ns_kernel bits */
#define NS_K_TUNABLES 0x01      /* protect_kernel_tunables */
#define NS_K_MODULES  0x02      /* protect_kernel_modules  */
#define NS_K_CGROUPS  0x04      /* protect_control_groups  */
#define NS_K_DEVICES  0x08      /* private_devices         */
#define NS_K_HOSTNAME 0x10      /* protect_hostname        */
#define NS_K_NETWORK  0x20      /* private_network           */
#define NS_K_MDWE     0x40      /* memory_deny_write_execute */
#define NS_K_COUNT    7
#define NS_K_MOUNT_BITS 0x1f    /* the ones apply_mount_ns builds */

/* .svc key of each NS_K_* bit, indexed by bit number */
extern const char *const ns_kernel_names[NS_K_COUNT];

/* "0" / "1" -> 0 / 1. Returns 0 on success, -1 on any other value. */
int parse_ns_bool(const char *val, uint8_t *out);

/* "0" / "1" / "full" -> PROTECT_SYSTEM_*. Returns 0 on success, -1 otherwise. */
int parse_protect_system(const char *val, uint8_t *out);

/* bounding-set caps the NS_K_* bits in kernel take away */
uint64_t ns_kernel_cap_drop(uint8_t kernel);

/* private_network: a new network namespace with loopback up. 0, or -1 + errno */
int private_net(void);

/* memory_deny_write_execute: PR_SET_MDWE, kept across execve. -1 + errno
 * (EINVAL before Linux 6.3) when the kernel can't */
int apply_mdwe(void);

#define MAX_RW_PATHS 8

/* Build the private mount view in the forked child, before caps are dropped.
 * name: the service's, for its private_tmp dir under /tmp/schema-private.
 * rw paths are made writable again inside the read-only mounts; a missing one
 * is skipped. No-op (0) when no mount knob and no NS_K_* bit is set. On failure returns
 * -1 with errno set and *step naming the failing operation. */
int apply_mount_ns(const char *name, uint8_t private_tmp, uint8_t protect_system,
                   uint8_t protect_home, uint8_t kernel, const char *const *rw, int n_rw,
                   const char **step);

#endif
