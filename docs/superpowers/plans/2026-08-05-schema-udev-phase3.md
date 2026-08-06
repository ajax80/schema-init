# schema-udev Phase 3 (mechanism) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two pure, byte-level-tested encoders to `schema-udev.h` — the libudev monitor wire format (group 2) and the `/run/udev/data` record format — wired to nothing in the live daemon.

**Architecture:** All new code is `static inline` in the pure-logic header `schema-udev.h` (the existing test seam), reached only from new unit tests. `schema-udev.c` is not touched. Correctness is anchored to two real captured libudev frames committed under `tests/fixtures/` and to non-circular murmur2 vectors taken from them.

**Tech Stack:** C99, gcc `-Wall -Wextra -D_GNU_SOURCE`, `<stdint.h>`/`<endian.h>` for fixed-width + endianness. Test runner is the repo `Makefile` `test` target (plain `assert`-based binaries).

## Global Constraints

- **`schema-udev.c` MUST remain byte-identical to its Phase 2 state.** No new call sites, no new sockets, no writes to real `/run/udev`. Verified in Task 3.
- **Endianness map (from captured golden frames — do not deviate):** in the libudev header, only `magic` and the four hash fields are **big-endian** (`htobe32`); `header_size`, `properties_off`, `properties_len` are **native** byte order.
- **Ground-truth murmur2 vectors (host order):** `murmur2("mem") == 0xc365cd83`, `murmur2("tty") == 0x8afa90c8`.
- **Header is exactly 40 bytes**: `char prefix[8]` (`"libudev\0"`) + 8×`uint32_t`. Serialize field-by-field into the buffer (do not rely on struct packing).
- **libudev magic** = `0xfeedcafe`.
- **All filesystem writers take an explicit `base_dir`** — tests pass `/tmp`; the real `/run/udev/data` is never passed.
- New code is `static inline` in `schema-udev.h`; follow the file's existing style (no comments unless clarifying a non-obvious invariant).
- Every task ends green under `make test` with `-Wall -Wextra` clean.

---

### Task 1: libudev monitor frame encoder (with murmur2)

**Files:**
- Modify: `schema-udev.h` (append before `#endif /* SCHEMA_UDEV_H */`)
- Create: `tests/test_libudev_frame.c`
- Uses fixture: `tests/fixtures/libudev-frame-mem-change.bin` (already committed)

**Interfaces:**
- Consumes: `struct uevent`, `uevent_get`, `safe_copy` (all existing in `schema-udev.h`).
- Produces:
  - `uint32_t murmur2(const char *str)`
  - `ssize_t libudev_frame_build(const struct uevent *ev, char *buf, size_t bufsz)` — returns total frame length (`40 + properties_len`) or `-1` if ACTION/DEVPATH/SUBSYSTEM missing or buffer too small.

- [ ] **Step 1: Write the failing test** — create `tests/test_libudev_frame.c`:

```c
#include "../schema-udev.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>

static void put(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

static uint32_t rd32(const char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(void) {
    /* 1. murmur2 ground-truth vectors (captured from real udev) */
    assert(murmur2("mem") == 0xc365cd83u);
    assert(murmur2("tty") == 0x8afa90c8u);

    /* 2. Build a frame for a synthetic mem/change event */
    struct uevent ev; memset(&ev, 0, sizeof ev);
    put(&ev, "ACTION", "change");
    put(&ev, "DEVPATH", "/devices/virtual/mem/null");
    put(&ev, "SUBSYSTEM", "mem");
    put(&ev, "DEVNAME", "/dev/null");
    put(&ev, "MAJOR", "1");
    put(&ev, "MINOR", "3");

    char buf[4096];
    ssize_t n = libudev_frame_build(&ev, buf, sizeof buf);
    assert(n > 40);

    assert(memcmp(buf, "libudev", 7) == 0 && buf[7] == '\0');
    assert(be32toh(rd32(buf + 8)) == 0xfeedcafeu);   /* magic: big-endian */
    assert(rd32(buf + 12) == 40);                    /* header_size: native */
    assert(rd32(buf + 16) == 40);                    /* properties_off: native */
    uint32_t plen = rd32(buf + 20);                  /* properties_len: native */
    assert((ssize_t)(40 + plen) == n);
    assert(be32toh(rd32(buf + 24)) == 0xc365cd83u);  /* subsystem hash: big-endian */
    assert(rd32(buf + 28) == 0);                     /* no DEVTYPE -> 0 */
    assert(rd32(buf + 32) == 0 && rd32(buf + 36) == 0); /* bloom hi/lo */

    /* 3. Self-decode round-trip: parse the properties back the libudev way */
    struct uevent back; memset(&back, 0, sizeof back);
    uint32_t off = rd32(buf + 16);
    uint32_t i = off;
    while (i < (uint32_t)n && back.n < UE_MAX_KEYS) {
        const char *rec = buf + i;
        size_t rl = strlen(rec);
        if (rl == 0) { i++; continue; }
        const char *eq = strchr(rec, '=');
        if (eq) {
            size_t kl = (size_t)(eq - rec);
            safe_copy(back.key[back.n], rec, kl + 1);
            safe_copy(back.val[back.n], eq + 1, UE_VAL_MAX);
            back.n++;
        }
        i += rl + 1;
    }
    assert(strcmp(uevent_get(&back, "ACTION"), "change") == 0);
    assert(strcmp(uevent_get(&back, "SUBSYSTEM"), "mem") == 0);
    assert(strcmp(uevent_get(&back, "DEVNAME"), "/dev/null") == 0);

    /* 4. The committed golden frame decodes to the documented header */
    FILE *f = fopen("tests/fixtures/libudev-frame-mem-change.bin", "rb");
    assert(f);
    char gold[8192];
    size_t glen = fread(gold, 1, sizeof gold, f);
    fclose(f);
    assert(glen >= 40);
    assert(memcmp(gold, "libudev", 7) == 0);
    assert(be32toh(rd32(gold + 8)) == 0xfeedcafeu);
    assert(rd32(gold + 12) == 40);
    assert(rd32(gold + 16) == 40);
    assert(be32toh(rd32(gold + 24)) == 0xc365cd83u);

    /* 5. Missing required key -> -1; tiny buffer -> -1 */
    struct uevent bad; memset(&bad, 0, sizeof bad);
    put(&bad, "ACTION", "add");   /* no DEVPATH/SUBSYSTEM */
    assert(libudev_frame_build(&bad, buf, sizeof buf) == -1);
    assert(libudev_frame_build(&ev, buf, 8) == -1);

    printf("test_libudev_frame: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it and confirm it fails to compile** (functions undefined):

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_libudev_frame.c -o /tmp/t && /tmp/t`
Expected: FAIL — `murmur2`/`libudev_frame_build` undefined.

- [ ] **Step 3: Implement in `schema-udev.h`** — append before the final `#endif`:

```c
#include <stdint.h>
#include <endian.h>

#define UDEV_MONITOR_MAGIC 0xfeedcafeu

static inline uint32_t murmur2(const char *str) {
    const uint32_t m = 0x5bd1e995u;
    const int r = 24;
    size_t len = strlen(str);
    const unsigned char *data = (const unsigned char *)str;
    uint32_t h = (uint32_t)len;   /* seed 0 */
    while (len >= 4) {
        uint32_t k = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                     ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k;
        data += 4; len -= 4;
    }
    switch (len) {
        case 3: h ^= (uint32_t)data[2] << 16; /* fall through */
        case 2: h ^= (uint32_t)data[1] << 8;  /* fall through */
        case 1: h ^= (uint32_t)data[0];
                h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

static inline ssize_t libudev_frame_build(const struct uevent *ev, char *buf, size_t bufsz) {
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    if (!uevent_get(ev, "ACTION") || !uevent_get(ev, "DEVPATH") || !sub) return -1;
    if (bufsz < 40) return -1;

    size_t plen = 0;
    for (int i = 0; i < ev->n; i++) {
        size_t klen = strlen(ev->key[i]);
        size_t vlen = strlen(ev->val[i]);
        size_t rec = klen + 1 + vlen + 1;   /* KEY=VALUE\0 */
        if (40 + plen + rec > bufsz) return -1;
        char *p = buf + 40 + plen;
        memcpy(p, ev->key[i], klen); p += klen;
        *p++ = '=';
        memcpy(p, ev->val[i], vlen); p += vlen;
        *p = '\0';
        plen += rec;
    }

    const char *devtype = uevent_get(ev, "DEVTYPE");
    memset(buf, 0, 40);
    memcpy(buf, "libudev", 7);   /* buf[7] left NUL by memset */
    uint32_t magic  = htobe32(UDEV_MONITOR_MAGIC);
    uint32_t hdrsz  = 40, poff = 40, plen32 = (uint32_t)plen;
    uint32_t subh   = htobe32(murmur2(sub));
    uint32_t dth    = devtype ? htobe32(murmur2(devtype)) : 0;
    memcpy(buf + 8,  &magic,  4);
    memcpy(buf + 12, &hdrsz,  4);
    memcpy(buf + 16, &poff,   4);
    memcpy(buf + 20, &plen32, 4);
    memcpy(buf + 24, &subh,   4);
    memcpy(buf + 28, &dth,    4);
    /* buf+32, buf+36 (bloom hi/lo) already zero */
    return (ssize_t)(40 + plen);
}
```

- [ ] **Step 4: Run the test, confirm it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_libudev_frame.c -o /tmp/t && /tmp/t`
Expected: `test_libudev_frame: OK`

- [ ] **Step 5: Commit**

```bash
git add schema-udev.h tests/test_libudev_frame.c
git commit -m "feat(schema-udev): libudev monitor frame encoder + murmur2 (Phase 3 mechanism)"
```

---

### Task 2: /run/udev/data record encoder

**Files:**
- Modify: `schema-udev.h` (append after Task 1's code, before `#endif`)
- Create: `tests/test_udev_db.c`

**Interfaces:**
- Consumes: `struct uevent`, `uevent_get` (existing). `<sys/stat.h>`, `<unistd.h>`, `<errno.h>` are already included by Phase 2's block in `schema-udev.h`.
- Produces:
  - `int udev_db_filename(const struct uevent *ev, char *out, size_t outsz)` — writes the db key (`c<maj>:<min>`, `b<maj>:<min>`, `n<ifindex>`, or `+<subsys>:<sysname>`); returns 0 / -1.
  - `ssize_t udev_db_record_build(const struct uevent *ev, char *buf, size_t bufsz)` — writes `V:1\n` then one `E:KEY=value\n` per property; returns length / -1.
  - `int udev_db_write(const char *base_dir, const struct uevent *ev)` — composes `base_dir/<filename>` and writes the record; returns 0 / -1.

- [ ] **Step 1: Write the failing test** — create `tests/test_udev_db.c`:

```c
#include "../schema-udev.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

int main(void) {
    char name[128];

    /* char device: mem/null -> c1:3 */
    struct uevent c; memset(&c, 0, sizeof c);
    put(&c, "SUBSYSTEM", "mem"); put(&c, "MAJOR", "1"); put(&c, "MINOR", "3");
    put(&c, "DEVPATH", "/devices/virtual/mem/null");
    assert(udev_db_filename(&c, name, sizeof name) == 0);
    assert(strcmp(name, "c1:3") == 0);

    /* block device -> b8:0 */
    struct uevent b; memset(&b, 0, sizeof b);
    put(&b, "SUBSYSTEM", "block"); put(&b, "MAJOR", "8"); put(&b, "MINOR", "0");
    assert(udev_db_filename(&b, name, sizeof name) == 0);
    assert(strcmp(name, "b8:0") == 0);

    /* net device -> n2 */
    struct uevent ndev; memset(&ndev, 0, sizeof ndev);
    put(&ndev, "SUBSYSTEM", "net"); put(&ndev, "IFINDEX", "2");
    assert(udev_db_filename(&ndev, name, sizeof name) == 0);
    assert(strcmp(name, "n2") == 0);

    /* no devnum -> +subsys:sysname (basename of DEVPATH) */
    struct uevent o; memset(&o, 0, sizeof o);
    put(&o, "SUBSYSTEM", "acpi"); put(&o, "DEVPATH", "/devices/LNXSYSTM:00/AMDI0030:00");
    assert(udev_db_filename(&o, name, sizeof name) == 0);
    assert(strcmp(name, "+acpi:AMDI0030:00") == 0);

    /* record contents */
    char rec[4096];
    ssize_t rn = udev_db_record_build(&c, rec, sizeof rec);
    assert(rn > 0);
    assert(strcmp(rec,
        "V:1\n"
        "E:SUBSYSTEM=mem\n"
        "E:MAJOR=1\n"
        "E:MINOR=3\n"
        "E:DEVPATH=/devices/virtual/mem/null\n") == 0);

    /* write to a /tmp base -> file named c1:3 with the record */
    char tmpl[] = "/tmp/schema-udev-db-XXXXXX";
    char *base = mkdtemp(tmpl);
    assert(base);
    assert(udev_db_write(base, &c) == 0);
    char path[256]; snprintf(path, sizeof path, "%s/c1:3", base);
    FILE *f = fopen(path, "r"); assert(f);
    char got[4096]; size_t gl = fread(got, 1, sizeof got - 1, f); got[gl] = '\0'; fclose(f);
    assert(strncmp(got, "V:1\n", 4) == 0);
    unlink(path); rmdir(base);

    /* overflow -> -1 */
    assert(udev_db_record_build(&c, rec, 3) == -1);

    printf("test_udev_db: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it and confirm it fails to compile** (functions undefined):

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_db.c -o /tmp/t && /tmp/t`
Expected: FAIL — `udev_db_filename`/`udev_db_record_build`/`udev_db_write` undefined.

- [ ] **Step 3: Implement in `schema-udev.h`** — append after Task 1's code:

```c
static inline int udev_db_filename(const struct uevent *ev, char *out, size_t outsz) {
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    const char *maj = uevent_get(ev, "MAJOR");
    const char *min = uevent_get(ev, "MINOR");
    const char *ifidx = uevent_get(ev, "IFINDEX");
    const char *devpath = uevent_get(ev, "DEVPATH");
    int w = -1;
    if (sub && strcmp(sub, "net") == 0 && ifidx)
        w = snprintf(out, outsz, "n%s", ifidx);
    else if (maj && min)
        w = snprintf(out, outsz, "%c%s:%s",
                     (sub && strcmp(sub, "block") == 0) ? 'b' : 'c', maj, min);
    else if (sub && devpath) {
        const char *slash = strrchr(devpath, '/');
        w = snprintf(out, outsz, "+%s:%s", sub, slash ? slash + 1 : devpath);
    } else
        return -1;
    return (w > 0 && (size_t)w < outsz) ? 0 : -1;
}

static inline ssize_t udev_db_record_build(const struct uevent *ev, char *buf, size_t bufsz) {
    int w = snprintf(buf, bufsz, "V:1\n");
    if (w < 0 || (size_t)w >= bufsz) return -1;
    size_t used = (size_t)w;
    for (int i = 0; i < ev->n; i++) {
        w = snprintf(buf + used, bufsz - used, "E:%s=%s\n", ev->key[i], ev->val[i]);
        if (w < 0 || (size_t)w >= bufsz - used) return -1;
        used += (size_t)w;
    }
    return (ssize_t)used;
}

static inline int udev_db_write(const char *base_dir, const struct uevent *ev) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    struct stat st;
    if (stat(base_dir, &st) != 0 && mkdir(base_dir, 0755) != 0 && errno != EEXIST) return -1;
    char path[512], buf[8192];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", base_dir, name) >= sizeof path) return -1;
    ssize_t len = udev_db_record_build(ev, buf, sizeof buf);
    if (len < 0) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fwrite(buf, 1, (size_t)len, f);
    fclose(f);
    return 0;
}
```

Note on record contents: the encoder emits `E:` for every property in `ev` in insertion order — that is why the test's expected string lists `SUBSYSTEM`, `MAJOR`, `MINOR`, `DEVPATH` in the order they were `put`. (Real udev omits kernel-implicit keys; filtering those is a future-cutover refinement, out of scope here.)

- [ ] **Step 4: Run the test, confirm it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_db.c -o /tmp/t && /tmp/t`
Expected: `test_udev_db: OK`

- [ ] **Step 5: Commit**

```bash
git add schema-udev.h tests/test_udev_db.c
git commit -m "feat(schema-udev): /run/udev/data record + filename encoders (Phase 3 mechanism)"
```

---

### Task 3: Wire tests into the build, document, and verify the safety boundary

**Files:**
- Modify: `Makefile` (the `test` target)
- Modify: `README.md`
- Verify (no change): `schema-udev.c`

**Interfaces:**
- Consumes: the two test files and encoders from Tasks 1–2.
- Produces: a green `make test` including the two new binaries, a README note, and a proof that `schema-udev.c` is unchanged.

- [ ] **Step 1: Add the new tests to the Makefile.** Open `Makefile`, find the `test` target (it currently compiles `tests/test_uevent_parse.c`, `tests/test_dev_match.c`, `tests/test_dev_load.c`, `tests/test_symlink.c`, `tests/test_coldplug.c` each as `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE <file> -o <out> && <out>`). Add two lines matching that exact pattern:

```make
	gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_libudev_frame.c -o /tmp/schema-test-libudev && /tmp/schema-test-libudev
	gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_db.c -o /tmp/schema-test-udevdb && /tmp/schema-test-udevdb
```

(Match the surrounding recipe's indentation — a literal TAB — and its output-binary naming convention.)

- [ ] **Step 2: Run the full suite, confirm all green**

Run: `make clean && make test`
Expected: every existing test plus `test_libudev_frame: OK` and `test_udev_db: OK`, no warnings.

- [ ] **Step 3: Add the README note.** In `README.md`, near the schema-udev / `.dev` section, add:

```markdown
### Phase 3 (interop mechanism — built, not yet active)

schema-udev carries pure encoders for the two formats a future udevd
retirement needs: the **libudev monitor** netlink frame (group 2) and the
**`/run/udev/data`** device-database record. They are unit-tested against
real captured frames but are **not wired into the daemon** — schema-udev
neither broadcasts on group 2 nor writes `/run/udev` while systemd-udevd
runs (doing so would double libudev events / corrupt udev's database).
Activating them is a separate, deliberate cutover, not part of this build.
```

- [ ] **Step 4: Verify the safety boundary — `schema-udev.c` is unchanged and references nothing new**

Run:
```bash
git diff --stat origin/master..HEAD -- schema-udev.c    # expect: no output (untouched)
grep -nE 'libudev_frame_build|udev_db_|murmur2' schema-udev.c   # expect: no matches
```
Expected: the first prints nothing; the second exits non-zero with no matches. If either shows output, the boundary is violated — stop and fix.

- [ ] **Step 5: VM boot regression test**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS`. (The daemon is unchanged, so this must stay green; it guards against a header include that broke the build.)

- [ ] **Step 6: Commit**

```bash
git add Makefile README.md
git commit -m "build(schema-udev): wire Phase 3 encoder tests + document mechanism boundary"
```

---

## Self-Review

**1. Spec coverage:**
- Feature A (libudev frame encoder + murmur2 + header layout + endianness + hashes) → Task 1. ✓
- Golden-fixture assertion + murmur ground-truth vectors → Task 1 steps 1/3. ✓
- Feature B (`udev_db_record_build`, `udev_db_filename`, `udev_db_write`, base_dir seam) → Task 2. ✓
- Makefile wiring, README note → Task 3. ✓
- Non-goal enforcement (`schema-udev.c` untouched, encoders test-only) → Task 3 Step 4 (explicit grep/diff gate). ✓
- vmtest PASS → Task 3 Step 5. ✓

**2. Placeholder scan:** No TBD/TODO/"handle edge cases"; every code step carries complete code. ✓

**3. Type consistency:** `murmur2` → `uint32_t`; `libudev_frame_build`/`udev_db_record_build` → `ssize_t` (len or -1); `udev_db_filename`/`udev_db_write` → `int` (0/-1). `put`/`rd32` test helpers are defined in each test file that uses them. Header offsets (8/12/16/20/24/28/32/36) consistent between the encoder and every decoding assertion. Fixture path `tests/fixtures/libudev-frame-mem-change.bin` matches the committed file. ✓
