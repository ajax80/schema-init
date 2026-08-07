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

static inline uint64_t hw_node_prefix(const struct hwdb *h, uint64_t noff) { return hw_le64(h->map + noff); }
static inline uint8_t  hw_node_cc(const struct hwdb *h, uint64_t noff)     { return h->map[noff + 8]; }
static inline uint64_t hw_node_vc(const struct hwdb *h, uint64_t noff)     { return hw_le64(h->map + noff + 16); }

static inline int hw_child(const struct hwdb *h, uint64_t noff, uint8_t idx,
                           uint8_t *c, uint64_t *coff) {
    uint64_t b = noff + h->node_size + (uint64_t)idx * h->child_entry_size;
    if (!hw_ok(h, b, h->child_entry_size)) return -1;
    *c = h->map[b];
    *coff = hw_le64(h->map + b + 8);
    return 0;
}

static inline int hw_child_lookup(const struct hwdb *h, uint64_t noff, uint8_t want, uint64_t *coff) {
    uint8_t cc = hw_node_cc(h, noff);
    for (uint8_t i = 0; i < cc; i++) {
        uint8_t c; uint64_t o;
        if (hw_child(h, noff, i, &c, &o) == 0 && c == want) { *coff = o; return 1; }
    }
    return 0;
}

struct hw_collect {
    char key[HW_MAX_PROPS][UE_KEY_MAX];
    char val[HW_MAX_PROPS][UE_VAL_MAX];
    uint16_t prio[HW_MAX_PROPS];
    uint32_t line[HW_MAX_PROPS];
    int n;
};

static inline void hw_collect_add(struct hw_collect *c, const char *k, const char *v,
                                  uint16_t prio, uint32_t line) {
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->key[i], k) == 0) {
            if (prio > c->prio[i] || (prio == c->prio[i] && line >= c->line[i])) {
                safe_copy(c->val[i], v, UE_VAL_MAX); c->prio[i] = prio; c->line[i] = line;
            }
            return;
        }
    }
    if (c->n < HW_MAX_PROPS) {
        safe_copy(c->key[c->n], k, UE_KEY_MAX);
        safe_copy(c->val[c->n], v, UE_VAL_MAX);
        c->prio[c->n] = prio; c->line[c->n] = line; c->n++;
    }
}

static inline void hw_add_value(const struct hwdb *h, uint64_t noff, uint8_t cc,
                                uint64_t idx, struct hw_collect *col) {
    uint64_t vb = noff + h->node_size + (uint64_t)cc * h->child_entry_size + idx * h->value_entry_size;
    if (!hw_ok(h, vb, h->value_entry_size)) return;
    uint64_t koff = hw_le64(h->map + vb);
    uint64_t voff = hw_le64(h->map + vb + 8);
    uint16_t prio = 0; uint32_t line = 0;
    if (h->value_entry_size >= 32) { line = hw_le32(h->map + vb + 24); prio = hw_le16(h->map + vb + 28); }
    const char *key = hw_string(h, koff);
    if (key[0] != ' ') return;
    hw_collect_add(col, key + 1, hw_string(h, voff), prio, line);
}

#endif /* SCHEMA_HWDB_H */
