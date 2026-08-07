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

int main(void) {
    test_open();
    test_values();
    printf("ALL hwdb tests passed\n");
    return 0;
}
