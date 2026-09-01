#ifndef SCHEMA_HWDB_H
#define SCHEMA_HWDB_H

#include "path_id.h"   /* pi_sysattr, safe_copy, struct uevent, UE_* */
#include <stdint.h>
#include <stdlib.h>
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

struct hw_linebuf { char b[HW_LINE_MAX]; size_t len; };
static inline void hw_lb_add(struct hw_linebuf *lb, const char *s, size_t n) {
    if (lb->len + n >= sizeof lb->b) return;
    memcpy(lb->b + lb->len, s, n); lb->len += n;
}
static inline void hw_lb_addc(struct hw_linebuf *lb, char c) {
    if (lb->len + 1 >= sizeof lb->b) return;
    lb->b[lb->len++] = c;
}
static inline void hw_lb_rem(struct hw_linebuf *lb, size_t n) { if (n <= lb->len) lb->len -= n; }

static inline void hw_fnmatch(const struct hwdb *h, uint64_t noff, size_t p,
                              struct hw_linebuf *lb, const char *search, struct hw_collect *col) {
    if (!hw_ok(h, noff, h->node_size)) return;
    const char *prefix = hw_string(h, hw_node_prefix(h, noff));
    size_t plen = strlen(prefix + p);
    hw_lb_add(lb, prefix + p, plen);
    uint8_t cc = hw_node_cc(h, noff);
    uint64_t vc = hw_node_vc(h, noff);
    for (uint8_t i = 0; i < cc; i++) {
        uint8_t c; uint64_t coff;
        if (hw_child(h, noff, i, &c, &coff) != 0) break;
        hw_lb_addc(lb, (char)c);
        hw_fnmatch(h, coff, 0, lb, search, col);
        hw_lb_rem(lb, 1);
    }
    if (vc) {
        char pat[HW_LINE_MAX];
        size_t L = lb->len < sizeof pat ? lb->len : sizeof pat - 1;
        memcpy(pat, lb->b, L); pat[L] = '\0';
        if (fnmatch(pat, search, 0) == 0)
            for (uint64_t i = 0; i < vc; i++) hw_add_value(h, noff, cc, i, col);
    }
    hw_lb_rem(lb, plen);
}

static inline void hw_search(const struct hwdb *h, const char *search, struct hw_collect *col) {
    struct hw_linebuf lb; lb.len = 0;
    uint64_t noff = h->root_off;
    size_t i = 0;
    while (hw_ok(h, noff, h->node_size)) {
        uint64_t poff = hw_node_prefix(h, noff);
        uint8_t cc = hw_node_cc(h, noff);
        uint64_t vc = hw_node_vc(h, noff);
        if (poff) {
            const char *prefix = hw_string(h, poff);
            size_t p = 0;
            for (; prefix[p]; p++) {
                char c = prefix[p];
                if (c == '*' || c == '?' || c == '[') { hw_fnmatch(h, noff, p, &lb, search + i + p, col); return; }
                if (c != search[i + p]) return;
            }
            i += p;
        }
        for (int g = 0; g < 3; g++) {
            char gc = (g == 0) ? '*' : (g == 1) ? '?' : '[';
            uint64_t coff;
            if (hw_child_lookup(h, noff, (uint8_t)gc, &coff)) {
                hw_lb_addc(&lb, gc);
                hw_fnmatch(h, coff, 0, &lb, search + i, col);
                hw_lb_rem(&lb, 1);
            }
        }
        if (search[i] == '\0') {
            for (uint64_t n = 0; n < vc; n++) hw_add_value(h, noff, cc, n, col);
            return;
        }
        uint64_t coff;
        if (!hw_child_lookup(h, noff, (uint8_t)search[i], &coff)) return;
        noff = coff; i++;
    }
}

static inline int hwdb_query(struct hwdb *h, const char *key, struct uevent *out) {
    struct hw_collect col; col.n = 0;
    hw_search(h, key, &col);
    for (int i = 0; i < col.n && out->n < UE_MAX_KEYS; i++) {
        safe_copy(out->key[out->n], col.key[i], UE_KEY_MAX);
        safe_copy(out->val[out->n], col.val[i], UE_VAL_MAX);
        out->n++;
    }
    return 0;
}

/* Synthesize systemd's `hwdb --subsystem=usb` modalias usb:vVVVVpPPPP:MODEL from a
 * usb_device's idVendor/idProduct/product sysattrs. usb_device nodes carry no
 * `modalias` sysattr (only usb_interface nodes do), so without this USB devices
 * resolve zero hwdb properties. Returns 0 on success, -1 if not a usb device. */
static inline int hwdb_usb_modalias(const char *syspath, char *out, size_t outsz) {
    char vend[64], prod[64], model[256];
    if (pi_sysattr(syspath, "idVendor", vend, sizeof vend) != 0) return -1;
    if (pi_sysattr(syspath, "idProduct", prod, sizeof prod) != 0) return -1;
    if (pi_sysattr(syspath, "product", model, sizeof model) != 0) model[0] = '\0';
    int w = snprintf(out, outsz, "usb:v%04Xp%04X:%s",
                     (unsigned)strtoul(vend, NULL, 16),
                     (unsigned)strtoul(prod, NULL, 16), model);
    return (w > 0 && (size_t)w < outsz) ? 0 : -1;
}

static inline int hwdb_build(const char *sysroot, const char *devpath, struct uevent *out) {
    out->n = 0;
    char syspath[PATH_MAX];
    if ((size_t)snprintf(syspath, sizeof syspath, "%s%s", sysroot, devpath) >= sizeof syspath) return 0;

    struct hwdb h;
    if (hwdb_open("/etc/udev/hwdb.bin", &h) != 0 &&
        hwdb_open("/usr/lib/udev/hwdb.bin", &h) != 0)
        return 0;

    char modalias[512];
    if (pi_sysattr(syspath, "modalias", modalias, sizeof modalias) == 0 && modalias[0])
        hwdb_query(&h, modalias, out);
    if (out->n == 0 && hwdb_usb_modalias(syspath, modalias, sizeof modalias) == 0)
        hwdb_query(&h, modalias, out);

    hwdb_close(&h);
    return 0;
}

#endif /* SCHEMA_HWDB_H */
