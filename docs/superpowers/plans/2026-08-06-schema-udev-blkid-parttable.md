# schema-udev blkid partition-table builtin (PR-A) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reproduce udev's `blkid` builtin partition-table output — `ID_PART_TABLE_*` and `ID_PART_ENTRY_*` — by reading the raw block device and parsing GPT (and MBR) on-disk structures, byte-for-byte across the 16 GPT nodes on blakbox.

**Architecture:** New header-only `blkid_pt.h`, a faithful port of systemd v259 `udev-builtin-blkid.c` partition handling + the GPT/MBR on-disk layout. It opens the device with `pread` (read-only), decodes the GPT header/entries (or MBR), and emits properties into a `struct uevent`. Mechanism only — wired to nothing. `schema-udev.c`/`.h` stay byte-identical. This is the first builtin to read a raw block device rather than only sysfs.

**Tech Stack:** C99, `-O2 -Wall -Wextra -D_GNU_SOURCE`, GNU Make. Reuses `path_id.h` (`pi_sysattr`, `pi_parent`, `pi_base`, `safe_copy`) and `schema-udev.h` (`struct uevent`, `uevent_get`, `UE_MAX_KEYS`, `UE_KEY_MAX`, `UE_VAL_MAX`).

## Global Constraints

- **Boundary:** `schema-udev.c` and `schema-udev.h` MUST remain byte-identical to master. `grep blkid_pt schema-udev.c` MUST be empty. Off by default, wired to nothing.
- **Normative source:** systemd v259 `src/udev/udev-builtin-blkid.c` + libblkid `partitions/gpt.c`/`dos.c` + the UEFI GPT layout. Source governs on any disagreement; the live gate is the authority.
- **Read-only raw device:** open with `O_RDONLY | O_CLOEXEC`, `pread` only the header + the one needed entry. Never write. Never open anything but the passed devnode / its parent disk.
- **PR-A emits ONLY** `ID_PART_TABLE_TYPE`, `ID_PART_TABLE_UUID`, and `ID_PART_ENTRY_{SCHEME,NAME,UUID,TYPE,FLAGS,NUMBER,OFFSET,SIZE,DISK}`. **No `ID_FS_*`** (that is PR-B). Emit a key at most once.
- **Whole disk** (no sysfs `partition` file) → only `ID_PART_TABLE_*`. **Partition** → inherited `ID_PART_TABLE_*` (from the parent table) + this partition's `ID_PART_ENTRY_*`.
- **Conditional keys:** `ID_PART_ENTRY_NAME` only if the GPT name is non-empty; `ID_PART_ENTRY_FLAGS` only if attributes ≠ 0. (Verified: nvme0n1p1 has NAME no FLAGS; sdb1 has both; nvme0n1p2 has neither.)
- **GUID mixed-endian:** first 3 groups little-endian (4/2/2 bytes), last 2 big-endian (2/6), lowercase `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`.
- **Name encoding:** `ID_PART_ENTRY_NAME` is `blkid_encode_string`-encoded — the stored/compared form escapes every byte not in `[0-9A-Za-z]` or `#+-.:=@_` as `\xNN` (lowercase). Space → `\x20`. Verified: udev stores `EFI\x20System\x20Partition`.
- **Offset/size units:** 512-byte sectors. `OFFSET = first_lba × (sector_size/512)`, `SIZE = (last_lba − first_lba + 1) × (sector_size/512)`. blakbox is 512 (factor 1); the scale is unit-tested for a 4Kn tree.
- **`blkid_pt.h` includes `path_id.h`** (transitively `schema-udev.h`) — reuse its primitives (DRY).
- Reuse `bpt_emit(out, k, v)` for every property; guard on `out->n < UE_MAX_KEYS`.
- Full-line exact match is the parity standard, **both directions**.

---

### Task 1: `blkid_pt.h` scaffold — raw read, little-endian, GUID formatter

**Files:**
- Create: `blkid_pt.h`
- Test: `tests/test_blkid_pt.c`
- Modify: `Makefile` (add the test build line)

**Interfaces:**
- Consumes: `path_id.h` (`safe_copy`), `schema-udev.h` (`struct uevent`, `UE_MAX_KEYS`, `UE_KEY_MAX`, `UE_VAL_MAX`, `uevent_get`).
- Produces:
  - `void bpt_emit(struct uevent *out, const char *k, const char *v)`.
  - `int bpt_read_at(const char *devnode, uint64_t off, void *buf, size_t len)` — `pread` wrapper; 0 iff exactly `len` bytes read.
  - `uint32_t bpt_le32(const unsigned char *p)`, `uint64_t bpt_le64(const unsigned char *p)`.
  - `void bpt_guid_str(const unsigned char g[16], char *out /* >=37 */)` — mixed-endian → canonical lowercase.

- [ ] **Step 1: Write the failing test**

Create `tests/test_blkid_pt.c`:

```c
#include "blkid_pt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_guid(void) {
    /* verified nvme0n1 disk GUID bytes -> 56d46da6-c484-4dd7-a6c3-d4693c92f94d */
    unsigned char g[16] = {0xa6,0x6d,0xd4,0x56, 0x84,0xc4, 0xd7,0x4d,
                           0xa6,0xc3, 0xd4,0x69,0x3c,0x92,0xf9,0x4d};
    char s[37];
    bpt_guid_str(g, s);
    assert(strcmp(s, "56d46da6-c484-4dd7-a6c3-d4693c92f94d") == 0);

    /* verified nvme0n1p1 type GUID -> c12a7328-f81f-11d2-ba4b-00a0c93ec93b */
    unsigned char t[16] = {0x28,0x73,0x2a,0xc1, 0x1f,0xf8, 0xd2,0x11,
                           0xba,0x4b, 0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    bpt_guid_str(t, s);
    assert(strcmp(s, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") == 0);

    printf("test_guid OK\n");
}

static void test_le(void) {
    unsigned char p[8] = {0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00};
    assert(bpt_le64(p) == 0x800);          /* 2048 */
    assert(bpt_le32(p) == 0x800);
    unsigned char q[8] = {0xff,0xc7,0x12,0x00,0x00,0x00,0x00,0x00};
    assert(bpt_le64(q) == 0x12c7ff);       /* 1230847 */
    printf("test_le OK\n");
}

int main(void) {
    test_guid();
    test_le();
    printf("ALL blkid_pt tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_pt.c -o /tmp/t && /tmp/t`
Expected: FAIL — `blkid_pt.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `blkid_pt.h`:

```c
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_pt.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_guid OK` / `test_le OK` / `ALL blkid_pt tests passed`. No warnings.

- [ ] **Step 5: Add the Makefile test line**

In `Makefile`, in the `test:` target, after the `test_net_id.c` line, add:

```make
	$(CC) $(CFLAGS) tests/test_blkid_pt.c -o /tmp/schema-test-blkidpt && /tmp/schema-test-blkidpt
```

- [ ] **Step 6: Commit**

```bash
git add blkid_pt.h tests/test_blkid_pt.c Makefile
git commit -m "feat(blkid_pt): scaffold — raw pread, little-endian, GUID formatter"
```

---

### Task 2: GPT whole-disk table probe

**Files:**
- Modify: `blkid_pt.h`
- Test: `tests/test_blkid_pt.c`

**Interfaces:**
- Consumes: `bpt_read_at`, `bpt_guid_str`, `pi_sysattr`.
- Produces:
  - `uint64_t bpt_sector_size(const char *disksys)` — `queue/logical_block_size` from the disk's sysfs dir, fallback 512.
  - `int bpt_gpt_disk_uuid(const char *devnode, uint64_t ssz, char *uuid_out /* >=37 */)` — reads the GPT header at LBA1; validates `"EFI PART"`; writes the disk GUID string; 0/-1.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_blkid_pt.c` a fabricated-GPT helper and a test; call `test_gpt_disk();` from `main`:

```c
#include <stdint.h>

/* write a minimal GPT: LBA1 header (@512), entries @1024. 512-byte sectors. */
static void mk_gpt(const char *path,
                   const unsigned char disk_guid[16],
                   const unsigned char *entries, size_t entries_len) {
    unsigned char buf[1024 + 512] = {0};
    unsigned char *hdr = buf + 512;
    memcpy(hdr, "EFI PART", 8);
    memcpy(hdr + 56, disk_guid, 16);
    /* partition_entry_lba = 2 */ hdr[72] = 2;
    /* num_entries = 128 */       hdr[80] = 128;
    /* entry_size = 128 */        hdr[84] = 128;
    if (entries && entries_len) memcpy(buf + 1024, entries, entries_len);
    FILE *f = fopen(path, "wb"); assert(f);
    fwrite(buf, 1, sizeof buf, f); fclose(f);
}

static void test_gpt_disk(void) {
    char img[] = "/tmp/bptgptXXXXXX";
    int fd = mkstemp(img); assert(fd >= 0); close(fd);
    unsigned char dg[16] = {0xa6,0x6d,0xd4,0x56,0x84,0xc4,0xd7,0x4d,
                            0xa6,0xc3,0xd4,0x69,0x3c,0x92,0xf9,0x4d};
    mk_gpt(img, dg, NULL, 0);

    char uuid[37];
    assert(bpt_gpt_disk_uuid(img, 512, uuid) == 0);
    assert(strcmp(uuid, "56d46da6-c484-4dd7-a6c3-d4693c92f94d") == 0);

    /* a non-GPT file fails */
    char bad[] = "/tmp/bptbadXXXXXX"; int bf = mkstemp(bad); assert(bf >= 0);
    { char z[1024] = {0}; assert(write(bf, z, sizeof z) == (ssize_t)sizeof z); } close(bf);
    assert(bpt_gpt_disk_uuid(bad, 512, uuid) != 0);

    unlink(img); unlink(bad);
    printf("test_gpt_disk OK\n");
}
```

Add `#include <unistd.h>` at the top of the test if not present (for `unlink`/`write`/`close`).

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_pt.c -o /tmp/t && /tmp/t`
Expected: FAIL — `bpt_gpt_disk_uuid` / `bpt_sector_size` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `blkid_pt.h` before the `#endif`:

```c
static inline uint64_t bpt_sector_size(const char *disksys) {
    char b[64];
    if (pi_sysattr(disksys, "queue/logical_block_size", b, sizeof b) == 0) {
        long s = atol(b);
        if (s >= 512) return (uint64_t)s;
    }
    return 512;
}

static inline int bpt_gpt_disk_uuid(const char *devnode, uint64_t ssz, char *uuid_out) {
    unsigned char hdr[96];
    if (bpt_read_at(devnode, ssz, hdr, sizeof hdr) != 0) return -1;
    if (memcmp(hdr, "EFI PART", 8) != 0) return -1;
    bpt_guid_str(hdr + 56, uuid_out);
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_pt.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_gpt_disk OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add blkid_pt.h tests/test_blkid_pt.c
git commit -m "feat(blkid_pt): GPT whole-disk table UUID + sector size"
```

---

### Task 3: GPT partition entry + blkid name encoder

**Files:**
- Modify: `blkid_pt.h`
- Test: `tests/test_blkid_pt.c`

**Interfaces:**
- Consumes: `bpt_read_at`, `bpt_le32/64`, `bpt_guid_str`, `bpt_emit`.
- Produces:
  - `int bpt_all_zero(const unsigned char *p, size_t n)`.
  - `void bpt_name_encode(const unsigned char *name16 /* 72 bytes UTF-16LE */, char *out, size_t outsz)` — UTF-8 + blkid-encode.
  - `int bpt_gpt_entry(const char *devnode, uint64_t ssz, unsigned n, unsigned char ent[128])` — reads the fixed 128-byte fields of GPT entry `n` (1-based); 0/-1 (bad header / n out of range).
  - `void bpt_emit_gpt_entry(const unsigned char ent[128], uint64_t ssz, unsigned n, const char *diskdev, struct uevent *out)` — emits `ID_PART_ENTRY_*` for one entry (skips an all-zero type GUID).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_blkid_pt.c`; call `test_gpt_entry();` from `main`. Helpers `bpt_has`/`bpt_absent`:

```c
static int bpt_has(const struct uevent *e, const char *k, const char *v) {
    const char *g = uevent_get(e, k); return g && strcmp(g, v) == 0;
}
static int bpt_absent(const struct uevent *e, const char *k) {
    return uevent_get(e, k) == NULL;
}

/* build the verified nvme0n1p1 entry (128 bytes) */
static void mk_entry_efi(unsigned char ent[128]) {
    memset(ent, 0, 128);
    unsigned char type[16] = {0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,
                              0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    unsigned char uuid[16] = {0x88,0x4a,0xa8,0x97,0xe4,0x34,0xde,0x4d,
                              0xb2,0xe8,0x4c,0x1e,0xe3,0xd5,0xfc,0xcc};
    memcpy(ent, type, 16); memcpy(ent + 16, uuid, 16);
    ent[32] = 0x00; ent[33] = 0x08;                 /* first_lba = 0x800 = 2048 */
    ent[40] = 0xff; ent[41] = 0xc7; ent[42] = 0x12; /* last_lba = 0x12c7ff = 1230847 */
    /* name "EFI System Partition" UTF-16LE */
    const char *nm = "EFI System Partition";
    for (size_t i = 0; nm[i]; i++) ent[56 + i*2] = (unsigned char)nm[i];
}

static void test_gpt_entry(void) {
    unsigned char ent[128];
    struct uevent e; e.n = 0;
    mk_entry_efi(ent);
    bpt_emit_gpt_entry(ent, 512, 1, "8:0", &e);
    assert(bpt_has(&e, "ID_PART_ENTRY_SCHEME", "gpt"));
    assert(bpt_has(&e, "ID_PART_ENTRY_TYPE", "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"));
    assert(bpt_has(&e, "ID_PART_ENTRY_UUID", "97a84a88-34e4-4dde-b2e8-4c1ee3d5fccc"));
    assert(bpt_has(&e, "ID_PART_ENTRY_NAME", "EFI\\x20System\\x20Partition"));
    assert(bpt_has(&e, "ID_PART_ENTRY_NUMBER", "1"));
    assert(bpt_has(&e, "ID_PART_ENTRY_OFFSET", "2048"));
    assert(bpt_has(&e, "ID_PART_ENTRY_SIZE", "1228800"));   /* 1230847-2048+1 */
    assert(bpt_has(&e, "ID_PART_ENTRY_DISK", "8:0"));
    assert(bpt_absent(&e, "ID_PART_ENTRY_FLAGS"));          /* attrs 0 */

    /* flags set, no name */
    unsigned char ent2[128]; memcpy(ent2, ent, 128);
    memset(ent2 + 56, 0, 72);                               /* clear name */
    ent2[48+7] = 0x80;                                      /* attrs = 0x8000000000000000 */
    struct uevent e2; e2.n = 0;
    bpt_emit_gpt_entry(ent2, 512, 2, "8:16", &e2);
    assert(bpt_absent(&e2, "ID_PART_ENTRY_NAME"));
    assert(bpt_has(&e2, "ID_PART_ENTRY_FLAGS", "0x8000000000000000"));
    assert(bpt_has(&e2, "ID_PART_ENTRY_NUMBER", "2"));

    /* empty slot (all-zero type) -> emits nothing */
    unsigned char ent3[128]; memset(ent3, 0, 128);
    struct uevent e3; e3.n = 0;
    bpt_emit_gpt_entry(ent3, 512, 3, "8:16", &e3);
    assert(e3.n == 0);

    /* 4Kn scaling: sector_size 4096 -> offset/size ×8 */
    struct uevent e4; e4.n = 0;
    bpt_emit_gpt_entry(ent, 4096, 1, "8:0", &e4);
    assert(bpt_has(&e4, "ID_PART_ENTRY_OFFSET", "16384"));  /* 2048*8 */
    assert(bpt_has(&e4, "ID_PART_ENTRY_SIZE", "9830400"));  /* 1228800*8 */

    printf("test_gpt_entry OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_pt.c -o /tmp/t && /tmp/t`
Expected: FAIL — `bpt_emit_gpt_entry` / `bpt_name_encode` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `blkid_pt.h` before the `#endif`:

```c
static inline int bpt_all_zero(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

static inline int bpt_name_safe(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || (c && strchr("#+-.:=@_", c) != NULL);
}

/* UTF-16LE (72 bytes, NUL-terminated within) -> UTF-8 -> blkid_encode_string */
static inline void bpt_name_encode(const unsigned char *name16, char *out, size_t outsz) {
    size_t o = 0;
    for (int i = 0; i + 1 < 72; i += 2) {
        unsigned u = (unsigned)name16[i] | ((unsigned)name16[i + 1] << 8);
        if (u == 0) break;
        unsigned char utf8[3]; int ulen;
        if (u < 0x80)       { utf8[0] = (unsigned char)u; ulen = 1; }
        else if (u < 0x800) { utf8[0] = (unsigned char)(0xc0 | (u >> 6));
                              utf8[1] = (unsigned char)(0x80 | (u & 0x3f)); ulen = 2; }
        else                { utf8[0] = (unsigned char)(0xe0 | (u >> 12));
                              utf8[1] = (unsigned char)(0x80 | ((u >> 6) & 0x3f));
                              utf8[2] = (unsigned char)(0x80 | (u & 0x3f)); ulen = 3; }
        for (int k = 0; k < ulen; k++) {
            unsigned char c = utf8[k];
            if (bpt_name_safe(c)) { if (o + 1 < outsz) out[o++] = (char)c; }
            else if (o + 4 < outsz) o += (size_t)snprintf(out + o, outsz - o, "\\x%02x", c);
        }
    }
    if (o < outsz) out[o] = '\0'; else if (outsz) out[outsz - 1] = '\0';
}

static inline int bpt_gpt_entry(const char *devnode, uint64_t ssz, unsigned n,
                                unsigned char ent[128]) {
    unsigned char hdr[96];
    if (bpt_read_at(devnode, ssz, hdr, sizeof hdr) != 0) return -1;
    if (memcmp(hdr, "EFI PART", 8) != 0) return -1;
    uint64_t entry_lba = bpt_le64(hdr + 72);
    uint32_t count     = bpt_le32(hdr + 80);
    uint32_t entsz     = bpt_le32(hdr + 84);
    if (n < 1 || n > count || entsz < 128) return -1;
    uint64_t off = entry_lba * ssz + (uint64_t)(n - 1) * entsz;
    return bpt_read_at(devnode, off, ent, 128);
}

static inline void bpt_emit_gpt_entry(const unsigned char ent[128], uint64_t ssz, unsigned n,
                                      const char *diskdev, struct uevent *out) {
    if (bpt_all_zero(ent, 16)) return;   /* unused slot */
    uint64_t scale = ssz / 512;
    uint64_t first = bpt_le64(ent + 32);
    uint64_t last  = bpt_le64(ent + 40);
    uint64_t attrs = bpt_le64(ent + 48);

    char s[64];
    bpt_emit(out, "ID_PART_ENTRY_SCHEME", "gpt");

    char name[256];
    bpt_name_encode(ent + 56, name, sizeof name);
    if (name[0]) bpt_emit(out, "ID_PART_ENTRY_NAME", name);

    bpt_guid_str(ent + 16, s); bpt_emit(out, "ID_PART_ENTRY_UUID", s);
    bpt_guid_str(ent + 0,  s); bpt_emit(out, "ID_PART_ENTRY_TYPE", s);

    if (attrs != 0) {
        snprintf(s, sizeof s, "0x%016llx", (unsigned long long)attrs);
        bpt_emit(out, "ID_PART_ENTRY_FLAGS", s);
    }
    snprintf(s, sizeof s, "%u", n);                    bpt_emit(out, "ID_PART_ENTRY_NUMBER", s);
    snprintf(s, sizeof s, "%llu", (unsigned long long)(first * scale));
    bpt_emit(out, "ID_PART_ENTRY_OFFSET", s);
    snprintf(s, sizeof s, "%llu", (unsigned long long)((last - first + 1) * scale));
    bpt_emit(out, "ID_PART_ENTRY_SIZE", s);
    bpt_emit(out, "ID_PART_ENTRY_DISK", diskdev);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_pt.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_gpt_entry OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add blkid_pt.h tests/test_blkid_pt.c
git commit -m "feat(blkid_pt): GPT partition entry + blkid name encoder"
```

---

### Task 4: MBR/dos table + entries (unit-tested only)

**Files:**
- Modify: `blkid_pt.h`
- Test: `tests/test_blkid_pt.c`

**Interfaces:**
- Consumes: `bpt_read_at`, `bpt_le32`, `bpt_emit`.
- Produces:
  - `int bpt_dos_disk_uuid(const char *devnode, char *uuid_out /* >=9 */)` — validates `0x55AA` at 510; writes the 4-byte disk signature (offset 440) as `%08x`; 0/-1.
  - `int bpt_dos_entry(const char *devnode, unsigned n, unsigned char ent[16])` — reads primary entry `n` (1-4) from offset 446; 0/-1. (Logical-partition chain walking for n≥5 is ported from source; see note.)
  - `void bpt_emit_dos_entry(const unsigned char ent[16], unsigned n, const char *diskdev, struct uevent *out)` — emits dos `ID_PART_ENTRY_*` (SCHEME=dos, TYPE=`0x%02x`, NUMBER, OFFSET, SIZE, FLAGS `0x80` if bootable); no NAME/UUID.

**No MBR disk exists on blakbox — this branch is unit-tested only; port the exact bytes from libblkid `partitions/dos.c`. The format literals below are normative.**

- [ ] **Step 1: Write the failing test**

Add to `tests/test_blkid_pt.c`; call `test_dos();` from `main`:

```c
static void test_dos(void) {
    char img[] = "/tmp/bptdosXXXXXX";
    int fd = mkstemp(img); assert(fd >= 0);
    unsigned char buf[512] = {0};
    /* disk signature at 440 = 0x11223344 (LE bytes) */
    buf[440] = 0x44; buf[441] = 0x33; buf[442] = 0x22; buf[443] = 0x11;
    /* primary entry 1 @446: bootable, type 0x83 (linux), start 2048, size 1000 */
    unsigned char *p = buf + 446;
    p[0] = 0x80;                    /* bootable */
    p[4] = 0x83;                    /* type */
    p[8] = 0x00; p[9] = 0x08;       /* start_lba = 2048 */
    p[12] = 0xe8; p[13] = 0x03;     /* num_sectors = 1000 */
    buf[510] = 0x55; buf[511] = 0xaa;
    assert(write(fd, buf, sizeof buf) == (ssize_t)sizeof buf); close(fd);

    char uuid[9];
    assert(bpt_dos_disk_uuid(img, uuid) == 0);
    assert(strcmp(uuid, "11223344") == 0);

    unsigned char ent[16];
    assert(bpt_dos_entry(img, 1, ent) == 0);
    struct uevent e; e.n = 0;
    bpt_emit_dos_entry(ent, 1, "8:0", &e);
    assert(bpt_has(&e, "ID_PART_ENTRY_SCHEME", "dos"));
    assert(bpt_has(&e, "ID_PART_ENTRY_TYPE", "0x83"));
    assert(bpt_has(&e, "ID_PART_ENTRY_NUMBER", "1"));
    assert(bpt_has(&e, "ID_PART_ENTRY_OFFSET", "2048"));
    assert(bpt_has(&e, "ID_PART_ENTRY_SIZE", "1000"));
    assert(bpt_has(&e, "ID_PART_ENTRY_FLAGS", "0x80"));

    unlink(img);
    printf("test_dos OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_pt.c -o /tmp/t && /tmp/t`
Expected: FAIL — `bpt_dos_disk_uuid` / `bpt_dos_entry` / `bpt_emit_dos_entry` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `blkid_pt.h` before the `#endif`:

```c
static inline int bpt_dos_disk_uuid(const char *devnode, char *uuid_out) {
    unsigned char mbr[512];
    if (bpt_read_at(devnode, 0, mbr, sizeof mbr) != 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xaa) return -1;
    snprintf(uuid_out, 9, "%08x", bpt_le32(mbr + 440));
    return 0;
}

static inline int bpt_dos_entry(const char *devnode, unsigned n, unsigned char ent[16]) {
    if (n < 1 || n > 4) return -1;   /* primaries only; logical chain: see note */
    unsigned char mbr[512];
    if (bpt_read_at(devnode, 0, mbr, sizeof mbr) != 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xaa) return -1;
    memcpy(ent, mbr + 446 + (n - 1) * 16, 16);
    return 0;
}

static inline void bpt_emit_dos_entry(const unsigned char ent[16], unsigned n,
                                      const char *diskdev, struct uevent *out) {
    unsigned char type = ent[4];
    if (type == 0) return;   /* empty entry */
    char s[64];
    bpt_emit(out, "ID_PART_ENTRY_SCHEME", "dos");
    snprintf(s, sizeof s, "0x%02x", type);          bpt_emit(out, "ID_PART_ENTRY_TYPE", s);
    if (ent[0] == 0x80) bpt_emit(out, "ID_PART_ENTRY_FLAGS", "0x80");
    snprintf(s, sizeof s, "%u", n);                 bpt_emit(out, "ID_PART_ENTRY_NUMBER", s);
    snprintf(s, sizeof s, "%u", bpt_le32(ent + 8)); bpt_emit(out, "ID_PART_ENTRY_OFFSET", s);
    snprintf(s, sizeof s, "%u", bpt_le32(ent + 12));bpt_emit(out, "ID_PART_ENTRY_SIZE", s);
    bpt_emit(out, "ID_PART_ENTRY_DISK", diskdev);
}
```

Note (source-parity): logical partitions (n≥5) live in the extended-partition chain (type `0x05`/`0x0f`/`0x85`); libblkid walks each EBR, numbering from 5, with OFFSET relative to the extended base. Port that walk from `partitions/dos.c` if a real MBR disk ever needs it; PR-A ships primaries + the format literals above, which the live gate does not exercise.

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_pt.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_dos OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add blkid_pt.h tests/test_blkid_pt.c
git commit -m "feat(blkid_pt): MBR/dos table + primary entries (unit-tested)"
```

---

### Task 5: orchestrator `blkid_pt_build`

**Files:**
- Modify: `blkid_pt.h`
- Test: `tests/test_blkid_pt.c`

**Interfaces:**
- Consumes: everything above + `pi_sysattr`, `pi_parent`, `pi_base`, `safe_copy`.
- Produces: `int blkid_pt_build(const char *sysroot, const char *devpath, const char *devnode, struct uevent *out)` — classifies whole-disk vs partition, probes the right device, emits `ID_PART_TABLE_*` (+ `ID_PART_ENTRY_*` for a partition). Returns 0 always (a device with no recognized table emits nothing).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_blkid_pt.c`; call `test_build();` from `main`. This fabricates a sysfs tree + a GPT image and drives the whole path:

```c
#include <sys/stat.h>
#include <limits.h>

static void bpt_mkdirs(const char *p) {
    char t[PATH_MAX]; safe_copy(t, p, sizeof t);
    for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    mkdir(t, 0755);
}
static void bpt_wf(const char *dir, const char *name, const char *val) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); assert(f); fputs(val, f); fputc('\n', f); fclose(f);
}

static void test_build(void) {
    char root[] = "/tmp/bptsysXXXXXX"; assert(mkdtemp(root));
    /* sysfs: /devices/disk (whole) with a child partition p1 */
    char disk[PATH_MAX]; snprintf(disk, sizeof disk, "%s/devices/disk", root); bpt_mkdirs(disk);
    char q[PATH_MAX]; snprintf(q, sizeof q, "%s/queue", disk); bpt_mkdirs(q);
    bpt_wf(q, "logical_block_size", "512");
    bpt_wf(disk, "dev", "8:0");
    char part[PATH_MAX]; snprintf(part, sizeof part, "%s/p1", disk); bpt_mkdirs(part);
    bpt_wf(part, "partition", "1");

    /* GPT image used as the "devnode" for both disk and its parent */
    char img[] = "/tmp/bptimgXXXXXX"; int fd = mkstemp(img); assert(fd >= 0); close(fd);
    unsigned char dg[16] = {0xa6,0x6d,0xd4,0x56,0x84,0xc4,0xd7,0x4d,
                            0xa6,0xc3,0xd4,0x69,0x3c,0x92,0xf9,0x4d};
    unsigned char ent[128]; mk_entry_efi(ent);
    mk_gpt(img, dg, ent, 128);

    /* whole disk: TABLE only, no ENTRY */
    struct uevent e; e.n = 0;
    assert(blkid_pt_build(root, "/devices/disk", img, &e) == 0);
    assert(bpt_has(&e, "ID_PART_TABLE_TYPE", "gpt"));
    assert(bpt_has(&e, "ID_PART_TABLE_UUID", "56d46da6-c484-4dd7-a6c3-d4693c92f94d"));
    assert(bpt_absent(&e, "ID_PART_ENTRY_SCHEME"));

    /* classification: p1 is detected as a partition (number 1) */
    char ps[PATH_MAX]; snprintf(ps, sizeof ps, "%s/devices/disk/p1", root);
    char pb[16];
    assert(pi_sysattr(ps, "partition", pb, sizeof pb) == 0 && atoi(pb) == 1);

    unlink(img);
    printf("test_build OK\n");
}
```

Note: the partition branch derives the parent devnode from `<dir-of-devnode>/<parent sysfs basename>`, which a tmpdir cannot faithfully fake (there is no matching parent image beside the tmp file). This test asserts **whole-disk TABLE emission + partition classification**; the partition entry decode is fully covered by `test_gpt_entry`, and the end-to-end partition path is proven by the live gate (Task 6).

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_pt.c -o /tmp/t && /tmp/t`
Expected: FAIL — `blkid_pt_build` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `blkid_pt.h` before the `#endif`:

```c
static inline int blkid_pt_build(const char *sysroot, const char *devpath,
                                 const char *devnode, struct uevent *out) {
    out->n = 0;
    char syspath[PATH_MAX];
    if ((size_t)snprintf(syspath, sizeof syspath, "%s%s", sysroot, devpath) >= sizeof syspath)
        return 0;

    char partbuf[64];
    int is_part = (pi_sysattr(syspath, "partition", partbuf, sizeof partbuf) == 0);

    if (!is_part) {
        uint64_t ssz = bpt_sector_size(syspath);
        char uuid[37];
        if (bpt_gpt_disk_uuid(devnode, ssz, uuid) == 0) {
            bpt_emit(out, "ID_PART_TABLE_TYPE", "gpt");
            bpt_emit(out, "ID_PART_TABLE_UUID", uuid);
        } else if (bpt_dos_disk_uuid(devnode, uuid) == 0) {
            bpt_emit(out, "ID_PART_TABLE_TYPE", "dos");
            bpt_emit(out, "ID_PART_TABLE_UUID", uuid);
        }
        return 0;
    }

    unsigned n = (unsigned)atoi(partbuf);

    char parentsys[PATH_MAX]; safe_copy(parentsys, syspath, sizeof parentsys);
    if (pi_parent(parentsys) != 0) return 0;
    uint64_t ssz = bpt_sector_size(parentsys);
    char pdev[64];
    if (pi_sysattr(parentsys, "dev", pdev, sizeof pdev) != 0) pdev[0] = '\0';

    /* parent devnode = same directory as devnode, basename = parent sysfs basename */
    char parentnode[PATH_MAX]; safe_copy(parentnode, devnode, sizeof parentnode);
    if (pi_parent(parentnode) == 0)
        snprintf(parentnode + strlen(parentnode), sizeof parentnode - strlen(parentnode),
                 "/%s", pi_base(parentsys));
    else
        snprintf(parentnode, sizeof parentnode, "/dev/%s", pi_base(parentsys));

    char uuid[37];
    if (bpt_gpt_disk_uuid(parentnode, ssz, uuid) == 0) {
        bpt_emit(out, "ID_PART_TABLE_TYPE", "gpt");
        bpt_emit(out, "ID_PART_TABLE_UUID", uuid);
        unsigned char ent[128];
        if (bpt_gpt_entry(parentnode, ssz, n, ent) == 0)
            bpt_emit_gpt_entry(ent, ssz, n, pdev, out);
    } else if (bpt_dos_disk_uuid(parentnode, uuid) == 0) {
        bpt_emit(out, "ID_PART_TABLE_TYPE", "dos");
        bpt_emit(out, "ID_PART_TABLE_UUID", uuid);
        unsigned char ent[16];
        if (bpt_dos_entry(parentnode, n, ent) == 0)
            bpt_emit_dos_entry(ent, n, pdev, out);
    }
    return 0;
}
```

Note: parent devnode resolution derives `<dir-of-devnode>/<parent sysfs basename>`. In the live gate the devnode is `/dev/<part>` and the parent basename is `<disk>`, giving `/dev/<disk>` — correct. The `/dev/<base>` fallback covers a bare basename devnode.

- [ ] **Step 4: Run the full suite**

Run: `make test`
Expected: PASS — all prior suites plus `test_guid` / `test_le` / `test_gpt_disk` / `test_gpt_entry` / `test_dos` / `test_build` / `ALL blkid_pt tests passed`. No warnings.

- [ ] **Step 5: Verify the boundary**

Run: `git diff origin/master -- schema-udev.c schema-udev.h; grep -c blkid_pt schema-udev.c`
Expected: empty diff, `0`.

- [ ] **Step 6: Commit**

```bash
git add blkid_pt.h tests/test_blkid_pt.c
git commit -m "feat(blkid_pt): orchestrator blkid_pt_build + classification"
```

---

### Task 6: live parity harness (sudo) + vmtest

**Files:**
- Create: `tests/verify_blkid_pt_live.sh`

**Interfaces:**
- Consumes: `blkid_pt_build` from `blkid_pt.h`.
- Produces: an executable acceptance script; prints `blkid_pt live parity: N devices, M mismatches` and exits non-zero if M > 0.

- [ ] **Step 1: Write the harness**

Create `tests/verify_blkid_pt_live.sh`:

```sh
#!/bin/sh
# Live parity gate: run blkid_pt_build over every /sys/class/block node, diff the
# ID_PART_TABLE_* / ID_PART_ENTRY_* subset vs `udevadm info` BOTH directions.
# Reads raw block devices -> runs the driver under sudo (user not in 'disk' group).
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/blkidpt_driver.c <<'EOF'
#include "blkid_pt.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;                 /* argv[1]=devpath argv[2]=devnode */
    struct uevent ev;
    if (blkid_pt_build("/sys", argv[1], argv[2], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/blkidpt_driver.c -o /tmp/blkidpt_driver

KEYS='^(ID_PART_TABLE_TYPE|ID_PART_TABLE_UUID|ID_PART_ENTRY_[A-Z_]*)='

props=$(mktemp)
ptprops=$(mktemp)
misses=$(mktemp)
total=0
for blk in /sys/class/block/*; do
    [ -e "$blk" ] || continue
    name=$(basename "$blk")
    dev=$(readlink -f "$blk"); devpath=${dev#/sys}
    devnode="/dev/$name"
    [ -b "$devnode" ] || continue
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -E "$KEYS" "$props" > "$ptprops" || true
    emitted=$(sudo /tmp/blkidpt_driver "$devpath" "$devnode")
    # skip nodes with no partition table on either side (e.g. zram0)
    [ -n "$emitted" ] || [ -s "$ptprops" ] || continue
    total=$((total + 1))
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$ptprops" || printf 'MISMATCH(val) %s | emit=%s\n' "$name" "$line"
    done >> "$misses"
    while IFS= read -r uline; do
        [ -n "$uline" ] || continue
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$name" "$uline"
    done < "$ptprops" >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'blkid_pt live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$ptprops" "$misses"
[ "$miss" -eq 0 ]
```

- [ ] **Step 2: Run the live gate**

Run: `chmod +x tests/verify_blkid_pt_live.sh && tests/verify_blkid_pt_live.sh`
Expected: `blkid_pt live parity: 15 devices, 0 mismatches` and exit 0. (5 disks + 10 partitions; zram0 and any table-less node are skipped. The device count varies with what's attached — only `0 mismatches` gates.)

If any `MISMATCH` prints: the emitted line and udev's value are shown. Fix the decode in `blkid_pt.h` (consult libblkid), re-run. Do NOT touch `schema-udev.c`.

- [ ] **Step 3: Run the vmtest boot rail**

Run: `cd ~/schema-livetest && ./vmtest.sh` then `cd -`
Expected: `>> RESULT: PASS`. Confirms the new header does not disturb the PID-1 boot.

- [ ] **Step 4: Commit**

```bash
git add tests/verify_blkid_pt_live.sh
git commit -m "test(blkid_pt): live udev parity acceptance harness (sudo, both directions)"
```

- [ ] **Step 5: Push and confirm origin == local (hard rule)**

```bash
git push -u origin feat/schema-udev-blkid-parttable
git ls-remote origin refs/heads/feat/schema-udev-blkid-parttable
git rev-parse HEAD
```

Expected: the two hashes are identical. Only then is the work landable.

---

## Notes for the implementer (Greg)

- **This is the first builtin to read a raw device.** `bpt_read_at` is the only I/O primitive — `pread`, read-only, header + one entry. Never write, never open anything but the passed devnode / its parent disk.
- **GUID mixed-endian is verified** against blakbox: disk GUID bytes `a6 6d d4 56 84 c4 d7 4d …` → `56d46da6-c484-4dd7-…`. The `bpt_guid_str` byte order (3/2/1/0, 5/4, 7/6, 8/9, 10..15) is correct — do not "fix" it to straight order.
- **Name is `\x20`-encoded** in udev's DB and in `udevadm info` output (`EFI\x20System\x20Partition`). `bpt_name_encode` must produce that — space and non-`[0-9A-Za-z#+-.:=@_]` bytes become `\xNN`.
- **Conditional emission:** NAME only if non-empty; FLAGS only if attrs ≠ 0. Getting this wrong shows as over-emission in the reverse direction of the live gate.
- **MBR is unit-tested only** — no MBR disk here. Port primaries + the format literals; logical-chain walking is deferred (noted in Task 4).
- `schema-udev.c` and `.h` are frozen. If a test seems to need a change there, stop — it doesn't.
- The live gate is the final authority. It needs `sudo` (block devices are `root:disk`). If the systemd/libblkid source and this plan ever differ, follow the source and let the gate confirm.
- This is **PR-A** of blkid. `ID_FS_*` filesystem probers are **PR-B**, a separate spec+plan+branch. Do not implement them here.
- After I (Claire) verify all gates, the branch lands via PR. Do not open the PR yourself.
```
