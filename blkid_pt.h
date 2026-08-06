#ifndef SCHEMA_BLKID_PT_H
#define SCHEMA_BLKID_PT_H

#include "path_id.h"   /* transitively: schema-udev.h (struct uevent, safe_copy) + pi_* helpers */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static inline void bpt_emit(struct uevent *out, const char *k, const char *v) {
    if (out->n < UE_MAX_KEYS) {
        safe_copy(out->key[out->n], k, UE_KEY_MAX);
        safe_copy(out->val[out->n], v, UE_VAL_MAX);
        out->n++;
    }
}

static inline int bpt_read_at(const char *devnode, uint64_t off, void *buf, size_t len) {
    int fd = open(devnode, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = pread(fd, buf, len, (off_t)off);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static inline uint32_t bpt_le32(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static inline uint64_t bpt_le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static inline void bpt_guid_str(const unsigned char g[16], char *out) {
    /* fields 1-3 little-endian (4/2/2), fields 4-5 big-endian (2/6) */
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

#endif /* SCHEMA_BLKID_PT_H */
