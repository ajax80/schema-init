# schema-udev hwdb builtin — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reproduce udev's `hwdb` builtin `*_FROM_DATABASE` output by reading systemd's compiled binary trie `hwdb.bin` and glob-matching a device's `modalias`, byte-for-byte across the ~199 modalias devices on blakbox.

**Architecture:** New header-only `hwdb.h`, a faithful port of systemd v259 `sd-hwdb` (`trie_search_f`, `trie_fnmatch_f`, `hwdb_add_property`). It `mmap`s the world-readable `hwdb.bin`, walks the trie matching the modalias (descending literally, recursing through `fnmatch()` at glob nodes), collects properties with the leading-space filter + priority dedup, and emits them into a `struct uevent`. Mechanism only — wired to nothing. `schema-udev.c`/`.h` stay byte-identical.

**Tech Stack:** C99, `-O2 -Wall -Wextra -D_GNU_SOURCE`, GNU Make. Reuses `path_id.h` (`pi_sysattr`, `safe_copy`) and `schema-udev.h` (`struct uevent`, `uevent_get`, `UE_*`). Uses libc `mmap`/`fnmatch`.

## Global Constraints

- **Boundary:** `schema-udev.c`/`.h` byte-identical to master. `grep hwdb schema-udev.c` empty. Off by default, wired to nothing.
- **Normative source:** systemd v259 `src/libsystemd/sd-hwdb/{hwdb-internal.h,sd-hwdb.c}`. Source governs; the live gate is the authority. The algorithm is validated end-to-end against blakbox's real `hwdb.bin` (the AMD root-complex modalias returns exactly udev's 4 properties).
- **Read-only, mmap:** open `hwdb.bin` `O_RDONLY`, `mmap` `PROT_READ`. World-readable → **no sudo**. All map accesses bounds-checked against `size`.
- **Binary format (verified):** header magic `KSLPHHRH`; LE-u64 fields at: `node_size`@32 (=24), `child_entry_size`@40 (=16), `value_entry_size`@48 (=32 ⇒ V2), `nodes_root_off`@56. Node = `prefix_off`(u64) + `children_count`(u8) + pad[7] + `values_count`(u64); children at `node+node_size` (sorted by byte), values after. Child = `c`(u8)+pad[7]+`child_off`(u64). V2 value = `key_off`(u64),`value_off`(u64),`filename_off`(u64),`line`(u32),`priority`(u16),pad(u16). All little-endian, read via byte-assembly (alignment-independent).
- **Key filter:** a value's key MUST start with `' '` (space); skip that space. Space-less keys are internal → ignored.
- **Priority dedup:** on a repeated property name, keep the incumbent unless the new entry has strictly higher `file_priority`, or equal priority and `line >= ` incumbent's line.
- **Lookup key = the device `modalias` sysattr.** No modalias → emit nothing. Values emitted verbatim (vendor strings contain spaces/commas/brackets).
- **`hwdb.h` includes `path_id.h`** — reuse its primitives (DRY). Glob via libc `fnmatch()`.
- Full-line exact match is the parity standard, **both directions**, on the `*_FROM_DATABASE` subset (excluding `ID_OUI_FROM_DATABASE`).

---

### Task 1: `hwdb.h` scaffold — open/mmap/header + LE readers + string + trie builder for tests

**Files:**
- Create: `hwdb.h`
- Test: `tests/test_hwdb.c`
- Modify: `Makefile` (add the test build line)

**Interfaces:**
- Consumes: `path_id.h` (`safe_copy`), `schema-udev.h` (`struct uevent`, `UE_*`).
- Produces:
  - `struct hwdb { const unsigned char *map; size_t size; uint64_t node_size, child_entry_size, value_entry_size, root_off; };`
  - `uint16_t hw_le16(const unsigned char*)`, `uint32_t hw_le32(...)`, `uint64_t hw_le64(...)`.
  - `int hw_ok(const struct hwdb*, uint64_t off, uint64_t len)` — bounds check.
  - `const char *hw_string(const struct hwdb*, uint64_t off)`.
  - `int hwdb_open(const char *path, struct hwdb *h)` — mmap + validate sig + parse header; 0/-1.
  - `void hwdb_close(struct hwdb *h)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_hwdb.c` (the trie builder helpers are reused by every later task):

```c
#include "hwdb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int hw_has(const struct uevent *e, const char *k, const char *v) {
    const char *g = uevent_get(e, k); return g && strcmp(g, v) == 0;
}
static int hw_absent(const struct uevent *e, const char *k) { return uevent_get(e, k) == NULL; }

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
static void add_child(uint8_t c, uint64_t child_off) {
    uint64_t o = IMGLEN; IMG[o] = c; pu64(o+8, child_off); IMGLEN += 16;
}
static void add_value(uint64_t koff, uint64_t voff, uint32_t line, uint16_t prio) {
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

int main(void) {
    test_open();
    printf("ALL hwdb tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_hwdb.c -o /tmp/t && /tmp/t`
Expected: FAIL — `hwdb.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `hwdb.h`:

```c
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_hwdb.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_open OK`. No warnings.

- [ ] **Step 5: Add the Makefile test line**

In `Makefile`, in the `test:` target, after the `test_blkid_fs.c` line, add:

```make
	$(CC) $(CFLAGS) tests/test_hwdb.c -o /tmp/schema-test-hwdb && /tmp/schema-test-hwdb
```

- [ ] **Step 6: Commit**

```bash
git add hwdb.h tests/test_hwdb.c Makefile
git commit -m "feat(hwdb): scaffold — mmap open, header parse, LE readers, string access"
```

---

### Task 2: node/child/value accessors + collector + `hw_add_value`

**Files:**
- Modify: `hwdb.h`
- Test: `tests/test_hwdb.c`

**Interfaces:**
- Consumes: `hw_le*`, `hw_ok`, `hw_string`, `safe_copy`.
- Produces:
  - `uint64_t hw_node_prefix(const struct hwdb*, uint64_t noff)`, `uint8_t hw_node_cc(...)`, `uint64_t hw_node_vc(...)`.
  - `int hw_child(const struct hwdb*, uint64_t noff, uint8_t idx, uint8_t *c, uint64_t *coff)`.
  - `int hw_child_lookup(const struct hwdb*, uint64_t noff, uint8_t want, uint64_t *coff)`.
  - `struct hw_collect { char key[HW_MAX_PROPS][UE_KEY_MAX]; char val[HW_MAX_PROPS][UE_VAL_MAX]; uint16_t prio[HW_MAX_PROPS]; uint32_t line[HW_MAX_PROPS]; int n; };`
  - `void hw_collect_add(struct hw_collect*, const char *k, const char *v, uint16_t prio, uint32_t line)` — dedup with priority tiebreak.
  - `void hw_add_value(const struct hwdb*, uint64_t noff, uint8_t cc, uint64_t idx, struct hw_collect*)` — reads a value entry, applies the leading-space filter, adds to the collector.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_hwdb.c`; call `test_values();` from `main`:

```c
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_hwdb.c -o /tmp/t && /tmp/t`
Expected: FAIL — `hw_node_cc` / `hw_add_value` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `hwdb.h` before the `#endif`:

```c
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_hwdb.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_values OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add hwdb.h tests/test_hwdb.c
git commit -m "feat(hwdb): node/child/value accessors + collector (leading-space filter, priority)"
```

---

### Task 3: `hw_fnmatch` + `hw_search` + `hwdb_query`

**Files:**
- Modify: `hwdb.h`
- Test: `tests/test_hwdb.c`

**Interfaces:**
- Consumes: node/child/value accessors, `hw_add_value`, `hw_collect_add`, libc `fnmatch`.
- Produces:
  - `struct hw_linebuf { char b[HW_LINE_MAX]; size_t len; };` + `hw_lb_add`/`hw_lb_addc`/`hw_lb_rem`.
  - `void hw_fnmatch(const struct hwdb*, uint64_t noff, size_t p, struct hw_linebuf*, const char *search, struct hw_collect*)`.
  - `void hw_search(const struct hwdb*, const char *search, struct hw_collect*)`.
  - `int hwdb_query(struct hwdb*, const char *key, struct uevent *out)` — search + emit each collected prop into `out` (cap `UE_MAX_KEYS`).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_hwdb.c`; call `test_search();` from `main`:

```c
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_hwdb.c -o /tmp/t && /tmp/t`
Expected: FAIL — `hwdb_query` / `hw_search` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `hwdb.h` before the `#endif`:

```c
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_hwdb.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_search OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add hwdb.h tests/test_hwdb.c
git commit -m "feat(hwdb): trie search + fnmatch glob recursion + hwdb_query"
```

---

### Task 4: `hwdb_build` orchestrator (modalias → default hwdb.bin → emit)

**Files:**
- Modify: `hwdb.h`
- Test: `tests/test_hwdb.c`

**Interfaces:**
- Consumes: `pi_sysattr`, `hwdb_open`, `hwdb_query`, `hwdb_close`.
- Produces: `int hwdb_build(const char *sysroot, const char *devpath, struct uevent *out)` — reads `<sysroot><devpath>/modalias`; opens `/etc/udev/hwdb.bin` (fallback `/usr/lib/udev/hwdb.bin`); queries; emits. Returns 0 always (no modalias / no db / no match ⇒ 0 keys).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_hwdb.c`; call `test_build();` from `main`:

```c
#include <sys/stat.h>
#include <limits.h>

static void test_build(void) {
    /* fabricate a sysfs node with a modalias; hwdb_build reads it (db open is exercised live) */
    char root[] = "/tmp/hwdbsysXXXXXX"; assert(mkdtemp(root));
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s/devices/x", root);
    { char t[PATH_MAX]; safe_copy(t, dir, sizeof t);
      for (char *s = t+1; *s; s++) if (*s=='/'){ *s=0; mkdir(t,0755); *s='/'; } mkdir(t,0755); }
    { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/modalias", dir);
      FILE *f = fopen(p, "w"); assert(f); fputs("pci:v0000ABCD\n", f); fclose(f); }

    /* no /etc/udev/hwdb.bin under our fake root: hwdb_build uses the real absolute path.
       On a box without hwdb.bin it simply emits nothing — assert it returns 0 and does not crash. */
    struct uevent e;
    assert(hwdb_build(root, "/devices/x", &e) == 0);

    /* a node with no modalias -> nothing */
    char dir2[PATH_MAX]; snprintf(dir2, sizeof dir2, "%s/devices/y", root);
    { char t[PATH_MAX]; safe_copy(t, dir2, sizeof t);
      for (char *s=t+1; *s; s++) if (*s=='/'){ *s=0; mkdir(t,0755); *s='/'; } mkdir(t,0755); }
    struct uevent e2;
    assert(hwdb_build(root, "/devices/y", &e2) == 0);
    assert(e2.n == 0);

    printf("test_build OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_hwdb.c -o /tmp/t && /tmp/t`
Expected: FAIL — `hwdb_build` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `hwdb.h` before the `#endif`:

```c
static inline int hwdb_build(const char *sysroot, const char *devpath, struct uevent *out) {
    out->n = 0;
    char syspath[PATH_MAX];
    if ((size_t)snprintf(syspath, sizeof syspath, "%s%s", sysroot, devpath) >= sizeof syspath) return 0;
    char modalias[512];
    if (pi_sysattr(syspath, "modalias", modalias, sizeof modalias) != 0) return 0;

    struct hwdb h;
    if (hwdb_open("/etc/udev/hwdb.bin", &h) != 0 &&
        hwdb_open("/usr/lib/udev/hwdb.bin", &h) != 0)
        return 0;
    hwdb_query(&h, modalias, out);
    hwdb_close(&h);
    return 0;
}
```

- [ ] **Step 4: Run the full suite**

Run: `make test`
Expected: PASS — all prior suites plus `test_open` / `test_values` / `test_search` / `test_build` / `ALL hwdb tests passed`. No warnings.

- [ ] **Step 5: Verify the boundary**

Run: `git diff origin/master -- schema-udev.c schema-udev.h; grep -c hwdb schema-udev.c`
Expected: empty diff, `0`.

- [ ] **Step 6: Commit**

```bash
git add hwdb.h tests/test_hwdb.c
git commit -m "feat(hwdb): orchestrator hwdb_build (modalias -> hwdb.bin -> emit)"
```

---

### Task 5: live parity harness + vmtest

**Files:**
- Create: `tests/verify_hwdb_live.sh`

**Interfaces:**
- Consumes: `hwdb_build` from `hwdb.h`.
- Produces: an executable acceptance script; prints `hwdb live parity: N devices, M mismatches` and exits non-zero if M > 0.

- [ ] **Step 1: Write the harness**

Create `tests/verify_hwdb_live.sh`:

```sh
#!/bin/sh
# Live parity gate: run hwdb_build over every /sys device with a modalias, diff the
# *_FROM_DATABASE subset vs `udevadm info` BOTH directions. ID_OUI_FROM_DATABASE is a
# composite OUI lookup (deferred) and is excluded. hwdb.bin is world-readable -> no sudo.
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/hwdb_driver.c <<'EOF'
#include "hwdb.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct uevent ev;
    if (hwdb_build("/sys", argv[1], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/hwdb_driver.c -o /tmp/hwdb_driver

KEYS='_FROM_DATABASE='
DEFER='^ID_OUI_FROM_DATABASE='

props=$(mktemp)
dbprops=$(mktemp)
misses=$(mktemp)
total=0
for dev in $(find /sys/devices -name modalias -printf '%h\n'); do
    devpath=${dev#/sys}
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -E "$KEYS" "$props" | grep -vE "$DEFER" > "$dbprops" || true
    emitted=$(/tmp/hwdb_driver "$devpath" | grep -vE "$DEFER" || true)
    [ -n "$emitted" ] || [ -s "$dbprops" ] || continue
    total=$((total + 1))
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$dbprops" || printf 'MISMATCH(val) %s | emit=%s\n' "$devpath" "$line"
    done >> "$misses"
    while IFS= read -r uline; do
        [ -n "$uline" ] || continue
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$devpath" "$uline"
    done < "$dbprops" >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'hwdb live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$dbprops" "$misses"
[ "$miss" -eq 0 ]
```

- [ ] **Step 2: Run the live gate**

Run: `chmod +x tests/verify_hwdb_live.sh && tests/verify_hwdb_live.sh`
Expected: `hwdb live parity: <N> devices, 0 mismatches` and exit 0 (N is the count of devices that carry a `*_FROM_DATABASE` property — ~150+ on blakbox; only `0 mismatches` gates).

If any `MISMATCH` prints: the emitted line and udev's value are shown. If it is a composite-lookup key (not a bare-modalias match, e.g. a new `*:...` prefix source), add it to `DEFER` as an out-of-scope composite lookup and note it; otherwise fix the trie walk in `hwdb.h` (consult `sd-hwdb.c`), re-run. Do NOT touch `schema-udev.c`.

- [ ] **Step 3: Run the vmtest boot rail**

Run: `cd ~/schema-livetest && ./vmtest.sh` then `cd -`
Expected: `>> RESULT: PASS`.

- [ ] **Step 4: Commit**

```bash
git add tests/verify_hwdb_live.sh
git commit -m "test(hwdb): live udev parity acceptance harness (both directions)"
```

- [ ] **Step 5: Push and confirm origin == local (hard rule)**

```bash
git push -u origin feat/schema-udev-hwdb
git ls-remote origin refs/heads/feat/schema-udev-hwdb
git rev-parse HEAD
```

Expected: the two hashes are identical. Only then is the work landable.

---

## Notes for the implementer (Greg)

- **The algorithm is validated end-to-end against blakbox's real `hwdb.bin`** — the AMD root-complex modalias returns exactly udev's four `*_FROM_DATABASE` properties. Port `trie_search_f`/`trie_fnmatch_f`/`hwdb_add_property` faithfully; the byte layout (`node_size=24`, `child_entry_size=16`, `value_entry_size=32`) is confirmed.
- **Leading-space key filter and priority dedup are load-bearing.** A value's key must start with a space (skip it); repeated names resolve by higher `file_priority`, then higher `line`.
- **Glob branches** (`*`,`?`,`[`) trigger `fnmatch()` on the accumulated linebuf pattern vs the remaining search string. The linebuf is bounded (`HW_LINE_MAX`); every map read is bounds-checked against `size`.
- **Lookup key = the device `modalias` sysattr** — nothing else for this scope. Composite-key lookups (evdev/mouse/keyboard/sensor/net:naming/OUI) are deferred to a future rules engine; the trie reader serves them via `hwdb_query` with a literal key (unit-tested).
- **No sudo** — `hwdb.bin` and sysfs `modalias` are world-readable.
- `schema-udev.c`/`.h` are frozen. If a test seems to need a change there, stop — it doesn't.
- The live gate is the final authority. If `sd-hwdb.c` and this plan ever differ, follow the source and let the gate confirm. If a live mismatch is a composite-lookup key (an out-of-scope `prefix:...` source), add it to the harness `DEFER` list rather than forcing a bare-modalias match.
- This is the **last builtin**. After I (Claire) verify all gates, the branch lands via PR. Do not open the PR yourself.
```
