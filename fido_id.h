#ifndef FIDO_ID_H
#define FIDO_ID_H

#include "path_id.h"    /* pi_parent (+ transitively schema-udev.h) */
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

static inline int fido_id_build(const char *sysroot, const char *devpath,
                                 struct uevent *out) {
    out->n = 0;
    char dir[PATH_MAX];
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", sysroot, devpath) >= sizeof dir) return 0;

    unsigned char buf[4096];
    ssize_t n = -1;
    for (;;) {
        char rd[PATH_MAX];
        if ((size_t)snprintf(rd, sizeof rd, "%s/report_descriptor", dir) < sizeof rd) {
            int fd = open(rd, O_RDONLY);
            if (fd >= 0) { n = read(fd, buf, sizeof buf); close(fd); if (n > 0) break; }
        }
        if (pi_parent(dir) != 0) break;
    }
    if (n < 3) return 0;

    int found = 0;
    for (ssize_t i = 0; i + 2 < n; i++)
        if (buf[i] == 0x06 && buf[i+1] == 0xd0 && buf[i+2] == 0xf1) { found = 1; break; }
    if (!found) return 0;

    #define FEMIT(k, v) do { \
        if (out->n < UE_MAX_KEYS) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], (v), UE_VAL_MAX); \
            out->n++; \
        } \
    } while (0)
    FEMIT("ID_FIDO_TOKEN", "1");
    FEMIT("ID_SECURITY_TOKEN", "1");
    #undef FEMIT
    return out->n;
}

#endif /* FIDO_ID_H */
