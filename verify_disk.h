#ifndef SCHEMA_VERIFY_DISK_H
#define SCHEMA_VERIFY_DISK_H

#include "verify_diff.h"
#include <mntent.h>

typedef enum {
    FS_LOCAL_REQ, FS_LOCAL_NOFAIL, FS_SWAP, FS_NETWORK, FS_VIRTUAL,
    FS_BIND, FS_AUTOMOUNT, FS_LAYERED, FS_NOAUTO
} fsclass_t;

fsclass_t fstab_class(const struct mntent *m);
/* Findings written up to MAX; returns how many there were in all. kmsg_text may be NULL. */
int disk_findings(const char *fstab_path, const char *kmsg_text, const char *devdir, finding_t *out, int max);

#endif
