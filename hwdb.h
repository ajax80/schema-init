#ifndef SCHEMA_HWDB_H
#define SCHEMA_HWDB_H

#include "path_id.h"   /* pi_sysattr, safe_copy, struct uevent, UE_* */
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fnmatch.h>

#define HW_MAX_PROPS 64
#define HW_LINE_MAX  2048

struct hwdb {
    const unsigned char *map;
    size_t size;
    uint64_t node_size, child_entry_size, value_entry_size, root_off;
};

static inline uint16_t hw_le16(const unsigned char *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static inline uint32_t hw_le32(const unsigned char *p) {
    uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); return v;
}
static inline uint64_t hw_le64(const unsigned char *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v;
}

static inline int hw_ok(const struct hwdb *h, uint64_t off, uint64_t len) {
    return off <= h->size && len <= h->size - off;
}
static inline const char *hw_string(const struct hwdb *h, uint64_t off) {
    if (off >= h->size) return "";
    return (const char *)(h->map + off);
}

static inline int hwdb_open(const char *path, struct hwdb *h) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 80) { close(fd); return -1; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    const unsigned char *b = (const unsigned char *)m;
    if (memcmp(b, "KSLPHHRH", 8) != 0) { munmap(m, (size_t)st.st_size); return -1; }
    h->map = b; h->size = (size_t)st.st_size;
    h->node_size        = hw_le64(b + 32);
    h->child_entry_size = hw_le64(b + 40);
    h->value_entry_size = hw_le64(b + 48);
    h->root_off         = hw_le64(b + 56);
    if (h->node_size < 24 || h->child_entry_size < 16 || h->value_entry_size < 16) {
        munmap(m, h->size); h->map = NULL; return -1;
    }
    return 0;
}

static inline void hwdb_close(struct hwdb *h) {
    if (h->map) { munmap((void *)h->map, h->size); h->map = NULL; }
}

#endif /* SCHEMA_HWDB_H */
