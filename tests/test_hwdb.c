#include "hwdb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

__attribute__((unused)) static int hw_has(const struct uevent *e, const char *k, const char *v) {
    const char *g = uevent_get(e, k); return g && strcmp(g, v) == 0;
}
__attribute__((unused)) static int hw_absent(const struct uevent *e, const char *k) { return uevent_get(e, k) == NULL; }

/* --- minimal in-memory hwdb.bin builder (the real 14MB file can't be a fixture) --- */
static unsigned char IMG[8192];
static size_t IMGLEN;
static void img_reset(void) { memset(IMG, 0, sizeof IMG); IMGLEN = 80; }   /* reserve header */
static void pu16(size_t at, uint16_t v) { IMG[at] = (unsigned char)v; IMG[at+1] = (unsigned char)(v>>8); }
static void pu32(size_t at, uint32_t v) { for (int i=0;i<4;i++) IMG[at+i] = (unsigned char)(v>>(8*i)); }
static void pu64(size_t at, uint64_t v) { for (int i=0;i<8;i++) IMG[at+i] = (unsigned char)(v>>(8*i)); }
static uint64_t put_str(const char *s) { uint64_t o = IMGLEN; memcpy(IMG+o, s, strlen(s)+1); IMGLEN += strlen(s)+1; return o; }
static uint64_t node_start(uint64_t prefix_off, uint8_t cc, uint64_t vc) {
    uint64_t o = IMGLEN; pu64(o, prefix_off); IMG[o+8] = cc; pu64(o+16, vc); IMGLEN += 24; return o;
}
__attribute__((unused)) static void add_child(uint8_t c, uint64_t child_off) {
    uint64_t o = IMGLEN; IMG[o] = c; pu64(o+8, child_off); IMGLEN += 16;
}
__attribute__((unused)) static void add_value(uint64_t koff, uint64_t voff, uint32_t line, uint16_t prio) {
    uint64_t o = IMGLEN;
    pu64(o, koff); pu64(o+8, voff); pu64(o+16, 0); pu32(o+24, line); pu16(o+28, prio); pu16(o+30, 0);
    IMGLEN += 32;
}
static char IMGPATH[64];
static const char *img_finalize(uint64_t root_off) {
    memcpy(IMG, "KSLPHHRH", 8);
    pu64(8, 259); pu64(16, IMGLEN); pu64(24, 80);
    pu64(32, 24); pu64(40, 16); pu64(48, 32);
    pu64(56, root_off); pu64(64, 0); pu64(72, 0);
    strcpy(IMGPATH, "/tmp/hwdbtestXXXXXX");
    int fd = mkstemp(IMGPATH); assert(fd >= 0);
    assert(write(fd, IMG, IMGLEN) == (ssize_t)IMGLEN); close(fd);
    return IMGPATH;
}

static void test_open(void) {
    img_reset();
    uint64_t pfx = put_str("hi");
    uint64_t root = node_start(pfx, 0, 0);
    const char *path = img_finalize(root);

    struct hwdb h;
    assert(hwdb_open(path, &h) == 0);
    assert(h.node_size == 24 && h.child_entry_size == 16 && h.value_entry_size == 32);
    assert(h.root_off == root);
    assert(strcmp(hw_string(&h, pfx), "hi") == 0);
    hwdb_close(&h);
    unlink(path);

    /* a non-hwdb file fails */
    char bad[] = "/tmp/hwdbbadXXXXXX"; int bf = mkstemp(bad); assert(bf >= 0);
    { char z[128] = {0}; assert(write(bf, z, sizeof z) == (ssize_t)sizeof z); } close(bf);
    struct hwdb h2;
    assert(hwdb_open(bad, &h2) != 0);
    unlink(bad);

    printf("test_open OK\n");
}

static void test_values(void) {
    img_reset();
    uint64_t pfx  = put_str("m");
    uint64_t k1   = put_str(" P1");        /* leading space -> emitted as P1 */
    uint64_t v1   = put_str("one");
    uint64_t knos = put_str("NOSPACE");    /* no leading space -> filtered */
    uint64_t v2   = put_str("two");
    uint64_t v3   = put_str("one_hi");
    /* root: prefix "m", 0 children, 3 values */
    uint64_t root = node_start(pfx, 0, 3);
    add_value(k1, v1, 0, 0);               /* P1=one  prio0 line0 */
    add_value(knos, v2, 0, 0);             /* filtered */
    add_value(k1, v3, 1, 5);               /* P1=one_hi prio5 -> wins */
    const char *path = img_finalize(root);

    struct hwdb h; assert(hwdb_open(path, &h) == 0);
    assert(hw_node_cc(&h, root) == 0 && hw_node_vc(&h, root) == 3);

    struct hw_collect col; col.n = 0;
    for (uint64_t i = 0; i < hw_node_vc(&h, root); i++)
        hw_add_value(&h, root, 0, i, &col);
    /* P1 present with the higher-priority value; NOSPACE filtered */
    int found_p1 = 0;
    for (int i = 0; i < col.n; i++) {
        if (strcmp(col.key[i], "P1") == 0) { found_p1 = 1; assert(strcmp(col.val[i], "one_hi") == 0); }
        assert(strcmp(col.key[i], "NOSPACE") != 0);
    }
    assert(found_p1 && col.n == 1);
    hwdb_close(&h); unlink(path);
    printf("test_values OK\n");
}

static void test_search(void) {
    /* trie1: root prefix "key" with 3 values (literal descent + filter + priority) */
    img_reset();
    uint64_t pfx = put_str("key");
    uint64_t k1 = put_str(" P1"), v1 = put_str("one"), v3 = put_str("one_hi");
    uint64_t kn = put_str("NOSPACE"), v2 = put_str("two");
    uint64_t root = node_start(pfx, 0, 3);
    add_value(k1, v1, 0, 0); add_value(kn, v2, 0, 0); add_value(k1, v3, 1, 5);
    const char *p1 = img_finalize(root);
    struct hwdb h; assert(hwdb_open(p1, &h) == 0);
    struct uevent e; e.n = 0;
    assert(hwdb_query(&h, "key", &e) == 0);
    assert(hw_has(&e, "P1", "one_hi") && hw_absent(&e, "NOSPACE") && e.n == 1);
    /* wrong search -> nothing */
    struct uevent e0; e0.n = 0; hwdb_query(&h, "nope", &e0); assert(e0.n == 0);
    hwdb_close(&h); unlink(p1);

    /* trie2: root prefix "x" with a '*' glob child -> value G (fnmatch path) */
    img_reset();
    uint64_t px = put_str("x");
    uint64_t empty = put_str("");           /* empty prefix for the glob child node */
    uint64_t gk = put_str(" G"), gv = put_str("globbed");
    uint64_t gnode = node_start(empty, 0, 1);     /* build leaf FIRST so its offset is known */
    add_value(gk, gv, 0, 0);
    uint64_t root2 = node_start(px, 1, 0);
    add_child('*', gnode);
    const char *p2 = img_finalize(root2);
    struct hwdb h2; assert(hwdb_open(p2, &h2) == 0);
    struct uevent e2; e2.n = 0;
    assert(hwdb_query(&h2, "xyz", &e2) == 0);
    assert(hw_has(&e2, "G", "globbed"));
    /* "x" alone also matches "*" of the remainder "" */
    struct uevent e3; e3.n = 0; hwdb_query(&h2, "x", &e3); assert(hw_has(&e3, "G", "globbed"));
    /* non-matching first char -> nothing */
    struct uevent e4; e4.n = 0; hwdb_query(&h2, "zzz", &e4); assert(e4.n == 0);
    hwdb_close(&h2); unlink(p2);

    printf("test_search OK\n");
}

#include <sys/stat.h>
#include <limits.h>

static void test_mkdirs(const char *p) {
    char t[PATH_MAX]; safe_copy(t, p, sizeof t);
    for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    mkdir(t, 0755);
}

static void test_build(void) {
    /* fabricate a sysfs node with a modalias; hwdb_build reads it (db open is exercised live) */
    char root[] = "/tmp/hwdbsysXXXXXX"; assert(mkdtemp(root));
    char dir[PATH_MAX];
    if ((size_t)snprintf(dir, sizeof dir, "%s/devices/x", root) >= sizeof dir) assert(0);
    test_mkdirs(dir);
    { char p[PATH_MAX];
      if ((size_t)snprintf(p, sizeof p, "%s/modalias", dir) >= sizeof p) assert(0);
      FILE *f = fopen(p, "w"); assert(f); fputs("pci:v0000ABCD\n", f); fclose(f); }

    /* no /etc/udev/hwdb.bin under our fake root: hwdb_build uses the real absolute path.
       On a box without hwdb.bin it simply emits nothing — assert it returns 0 and does not crash. */
    struct uevent e;
    assert(hwdb_build(root, "/devices/x", &e) == 0);

    /* a node with no modalias -> nothing */
    char dir2[PATH_MAX];
    if ((size_t)snprintf(dir2, sizeof dir2, "%s/devices/y", root) >= sizeof dir2) assert(0);
    test_mkdirs(dir2);
    struct uevent e2;
    assert(hwdb_build(root, "/devices/y", &e2) == 0);
    assert(e2.n == 0);

    printf("test_build OK\n");
}

int main(void) {
    test_open();
    test_values();
    test_search();
    test_build();
    printf("ALL hwdb tests passed\n");
    return 0;
}
