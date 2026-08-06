# schema-udev blkid filesystem probers (PR-B) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reproduce udev's `blkid` builtin identity `ID_FS_*` output — TYPE/USAGE/UUID(_ENC)/LABEL(_ENC)/UUID_SUB(_ENC)/VERSION — for ext4, btrfs, vfat, ntfs, exfat, swap, byte-for-byte across the 12 formatted nodes on blakbox.

**Architecture:** New header-only `blkid_fs.h`, a faithful port of systemd v259 `udev-builtin-blkid.c` + util-linux libblkid superblock probers. It reuses PR-A's `blkid_pt.h` raw-read/little-endian/emit primitives, probes each FS by magic in libblkid order, and emits identity properties into a `struct uevent`. Mechanism only — wired to nothing. `schema-udev.c`/`.h` stay byte-identical.

**Tech Stack:** C99, `-O2 -Wall -Wextra -D_GNU_SOURCE`, GNU Make. Reuses `blkid_pt.h` (`bpt_read_at`, `bpt_le16/32/64`, `bpt_emit`, `bpt_all_zero`, `bpt_name_safe`) and `schema-udev.h` (`struct uevent`, `uevent_get`, `UE_*`).

## Global Constraints

- **Boundary:** `schema-udev.c`/`.h` byte-identical to master. `grep blkid_fs schema-udev.c` empty. Off by default, wired to nothing.
- **Normative source:** systemd v259 `udev-builtin-blkid.c` + util-linux libblkid `superblocks/{ext,btrfs,vfat,ntfs,exfat,swap}.c`. Source governs; the live gate is the authority.
- **Read-only raw device:** all I/O via `bpt_read_at` (`pread`). Never write.
- **Identity fields only.** Emit `ID_FS_TYPE`, `ID_FS_USAGE`, `ID_FS_UUID(_ENC)`, `ID_FS_LABEL(_ENC)`, `ID_FS_UUID_SUB(_ENC)`, `ID_FS_VERSION`. **Never** emit `ID_FS_SIZE`/`ID_FS_BLOCKSIZE`/`ID_FS_LASTBLOCK` (deferred) or any `ID_PART_*` (PR-A). Emit a key at most once.
- **First-match-wins dispatch:** btrfs → ext → ntfs → exfat → vfat → swap. ntfs/exfat are checked before vfat because all three carry the `0x55AA` boot signature; the offset-3 OEM magic disambiguates.
- **FS UUIDs are STRAIGHT-byte** — use `fs_uuid_straight` (bytes in order), NOT PR-A's `bpt_guid_str`. Verified: ext4 bytes `cf 4f 2b 07 …` → `cf4f2b07-…`.
- **`ID_FS_UUID`/`LABEL` = `blkid_safe_string`; `_ENC` = `blkid_encode_string`** (`\xNN` for bytes outside `[0-9A-Za-z#+-.:=@_]`). UUID strings are already safe → both forms equal; labels differ only for non-ASCII/space.
- **Conditional LABEL:** emit only if non-empty (and not the vfat `NO NAME    ` sentinel). **`UUID_SUB` only for btrfs.**
- **`blkid_fs.h` includes `blkid_pt.h`** (on master) — reuse its primitives (DRY).
- Full-line exact match is the parity standard, **both directions**, on the identity subset.

---

### Task 1: `blkid_fs.h` scaffold — UUID/label formatters + encoders

**Files:**
- Create: `blkid_fs.h`
- Test: `tests/test_blkid_fs.c`
- Modify: `Makefile` (add the test build line)

**Interfaces:**
- Consumes: `blkid_pt.h` (`bpt_read_at`, `bpt_le16/32/64`, `bpt_emit`, `bpt_all_zero`, `bpt_name_safe`), `schema-udev.h` (`struct uevent`, `uevent_get`).
- Produces:
  - `void fs_uuid_straight(const unsigned char g[16], char *out /* >=37 */)`.
  - `void fs_safe_bytes(const unsigned char *in, size_t inlen, char *out, size_t outsz)` — blkid_safe_string.
  - `void fs_encode_bytes(const unsigned char *in, size_t inlen, char *out, size_t outsz)` — blkid_encode_string.
  - `void fs_utf16_to_utf8(const unsigned char *in, size_t bytelen, char *out, size_t outsz)`.
  - `void fs_emit_uuid(struct uevent *out, const char *uuid)` — emits `ID_FS_UUID` + `ID_FS_UUID_ENC`.
  - `void fs_emit_label(struct uevent *out, const unsigned char *raw, size_t len)` — emits `ID_FS_LABEL` + `_ENC` if non-empty (stops at NUL).

- [ ] **Step 1: Write the failing test**

Create `tests/test_blkid_fs.c`:

```c
#include "blkid_fs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fs_has(const struct uevent *e, const char *k, const char *v) {
    const char *g = uevent_get(e, k); return g && strcmp(g, v) == 0;
}
static int fs_absent(const struct uevent *e, const char *k) { return uevent_get(e, k) == NULL; }

static void test_helpers(void) {
    unsigned char g[16] = {0xcf,0x4f,0x2b,0x07,0xf1,0x50,0x40,0x4f,
                           0xbd,0xe1,0x4d,0x9f,0x54,0x55,0x94,0xb4};
    char s[37]; fs_uuid_straight(g, s);
    assert(strcmp(s, "cf4f2b07-f150-404f-bde1-4d9f545594b4") == 0);

    /* encode: space -> \x20, alnum kept */
    char enc[64]; fs_encode_bytes((const unsigned char *)"My Disk", 7, enc, sizeof enc);
    assert(strcmp(enc, "My\\x20Disk") == 0);
    /* safe: space kept, '/' -> _ */
    char safe[64]; fs_safe_bytes((const unsigned char *)"a/b c", 5, safe, sizeof safe);
    assert(strcmp(safe, "a_b c") == 0);

    /* utf16 -> utf8 (ASCII "RECOVERY") */
    unsigned char u16[16] = {'R',0,'E',0,'C',0,'O',0,'V',0,'E',0,'R',0,'Y',0};
    char u8[32]; fs_utf16_to_utf8(u16, 16, u8, sizeof u8);
    assert(strcmp(u8, "RECOVERY") == 0);

    /* emit label */
    struct uevent e; e.n = 0;
    fs_emit_label(&e, (const unsigned char *)"fedora", 6);
    assert(fs_has(&e, "ID_FS_LABEL", "fedora") && fs_has(&e, "ID_FS_LABEL_ENC", "fedora"));
    struct uevent e2; e2.n = 0;
    fs_emit_label(&e2, (const unsigned char *)"", 0);
    assert(fs_absent(&e2, "ID_FS_LABEL"));

    printf("test_helpers OK\n");
}

int main(void) {
    test_helpers();
    printf("ALL blkid_fs tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: FAIL — `blkid_fs.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `blkid_fs.h`:

```c
#ifndef SCHEMA_BLKID_FS_H
#define SCHEMA_BLKID_FS_H

#include "blkid_pt.h"   /* bpt_read_at, bpt_le*, bpt_emit, bpt_all_zero, bpt_name_safe (+ path_id/schema-udev) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline void fs_uuid_straight(const unsigned char g[16], char *out) {
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static inline void fs_safe_bytes(const unsigned char *in, size_t inlen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < inlen && in[i]; i++) {
        if (o + 1 >= outsz) break;
        unsigned char c = in[i];
        out[o++] = (c < 0x20 || c == 0x7f || c == '/' || c == '\\') ? '_' : (char)c;
    }
    out[o] = '\0';
}

static inline void fs_encode_bytes(const unsigned char *in, size_t inlen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < inlen && in[i]; i++) {
        unsigned char c = in[i];
        if (bpt_name_safe(c)) { if (o + 1 < outsz) out[o++] = (char)c; }
        else if (o + 4 < outsz) o += (size_t)snprintf(out + o, outsz - o, "\\x%02x", c);
    }
    out[o] = '\0';
}

static inline void fs_utf16_to_utf8(const unsigned char *in, size_t bytelen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i + 1 < bytelen; i += 2) {
        unsigned u = (unsigned)in[i] | ((unsigned)in[i + 1] << 8);
        if (u == 0) break;
        if (u < 0x80)       { if (o + 1 < outsz) out[o++] = (char)u; }
        else if (u < 0x800) { if (o + 2 < outsz) { out[o++] = (char)(0xc0 | (u >> 6));
                                                   out[o++] = (char)(0x80 | (u & 0x3f)); } }
        else                { if (o + 3 < outsz) { out[o++] = (char)(0xe0 | (u >> 12));
                                                   out[o++] = (char)(0x80 | ((u >> 6) & 0x3f));
                                                   out[o++] = (char)(0x80 | (u & 0x3f)); } }
    }
    if (o < outsz) out[o] = '\0'; else if (outsz) out[outsz - 1] = '\0';
}

static inline void fs_emit_uuid(struct uevent *out, const char *uuid) {
    bpt_emit(out, "ID_FS_UUID", uuid);
    bpt_emit(out, "ID_FS_UUID_ENC", uuid);
}

static inline void fs_emit_label(struct uevent *out, const unsigned char *raw, size_t len) {
    size_t n = 0; while (n < len && raw[n]) n++;
    if (n == 0) return;
    char safe[256], enc[256];
    fs_safe_bytes(raw, n, safe, sizeof safe);
    fs_encode_bytes(raw, n, enc, sizeof enc);
    if (safe[0]) bpt_emit(out, "ID_FS_LABEL", safe);
    if (enc[0])  bpt_emit(out, "ID_FS_LABEL_ENC", enc);
}

#endif /* SCHEMA_BLKID_FS_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_helpers OK`. No warnings.

- [ ] **Step 5: Add the Makefile test line**

In `Makefile`, in the `test:` target, after the `test_blkid_pt.c` line, add:

```make
	$(CC) $(CFLAGS) tests/test_blkid_fs.c -o /tmp/schema-test-blkidfs && /tmp/schema-test-blkidfs
```

- [ ] **Step 6: Commit**

```bash
git add blkid_fs.h tests/test_blkid_fs.c Makefile
git commit -m "feat(blkid_fs): scaffold — straight UUID, blkid safe/encode, label emit"
```

---

### Task 2: ext2/3/4 prober

**Files:**
- Modify: `blkid_fs.h`
- Test: `tests/test_blkid_fs.c`

**Interfaces:**
- Consumes: `bpt_read_at`, `bpt_le16/32`, `fs_uuid_straight`, `fs_emit_uuid`, `fs_emit_label`, `bpt_emit`.
- Produces: `int fs_probe_ext(const char *dev, struct uevent *out)` — magic `0xEF53` at sb+56 (sb@1024); emits TYPE (ext2/ext3/ext4 by feature flags), USAGE=filesystem, UUID (straight @sb+104), LABEL (@sb+120, 16B), VERSION `%u.%u`; 0 on match, -1 otherwise.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_blkid_fs.c` a superblock-image helper + test; call `test_ext();` from `main`:

```c
#include <stdint.h>

static void wr_img(const char *path, uint64_t off, const unsigned char *buf, size_t len) {
    FILE *f = fopen(path, "r+b"); if (!f) f = fopen(path, "w+b"); assert(f);
    fseek(f, (long)off, SEEK_SET); fwrite(buf, 1, len, f); fclose(f);
}
static void zero_img(const char *path, size_t size) {
    FILE *f = fopen(path, "wb"); assert(f);
    unsigned char z[512] = {0};
    for (size_t i = 0; i < size; i += sizeof z) fwrite(z, 1, sizeof z, f);
    fclose(f);
}

static void test_ext(void) {
    char img[] = "/tmp/fsextXXXXXX"; int fd = mkstemp(img); assert(fd >= 0); close(fd);
    zero_img(img, 4096);
    unsigned char sb[264] = {0};
    sb[56] = 0x53; sb[57] = 0xef;                         /* magic 0xEF53 */
    unsigned char uu[16] = {0xcf,0x4f,0x2b,0x07,0xf1,0x50,0x40,0x4f,
                            0xbd,0xe1,0x4d,0x9f,0x54,0x55,0x94,0xb4};
    memcpy(sb + 104, uu, 16);                             /* s_uuid */
    /* no label (sb+120 stays zero) */
    sb[0x4C] = 1;                                         /* s_rev_level = 1 */
    /* s_minor_rev_level (u16 @ 0x7E) = 0 */
    sb[0x60] = 0x40;                                      /* incompat EXTENTS -> ext4 */
    wr_img(img, 1024, sb, sizeof sb);

    struct uevent e; e.n = 0;
    assert(fs_probe_ext(img, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "ext4"));
    assert(fs_has(&e, "ID_FS_USAGE", "filesystem"));
    assert(fs_has(&e, "ID_FS_UUID", "cf4f2b07-f150-404f-bde1-4d9f545594b4"));
    assert(fs_has(&e, "ID_FS_UUID_ENC", "cf4f2b07-f150-404f-bde1-4d9f545594b4"));
    assert(fs_has(&e, "ID_FS_VERSION", "1.0"));
    assert(fs_absent(&e, "ID_FS_LABEL"));

    unlink(img);
    printf("test_ext OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: FAIL — `fs_probe_ext` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `blkid_fs.h` before the `#endif`:

```c
static inline int fs_probe_ext(const char *dev, struct uevent *out) {
    unsigned char sb[264];
    if (bpt_read_at(dev, 1024, sb, sizeof sb) != 0) return -1;
    if (!(sb[56] == 0x53 && sb[57] == 0xef)) return -1;   /* 0xEF53 LE */

    uint32_t fc = bpt_le32(sb + 0x5C);   /* feature_compat   */
    uint32_t fi = bpt_le32(sb + 0x60);   /* feature_incompat */
    uint32_t frc = bpt_le32(sb + 0x64);  /* feature_ro_compat */
    const char *type;
    /* ext4 markers: EXTENTS|64BIT|FLEX_BG (incompat) or HUGE_FILE|GDT_CSUM|DIR_NLINK|EXTRA_ISIZE|METADATA_CSUM (ro) */
    if ((fi & (0x0040u | 0x0080u | 0x0200u)) ||
        (frc & (0x0008u | 0x0010u | 0x0020u | 0x0040u | 0x0400u)))
        type = "ext4";
    else if (fc & 0x0004u)   /* HAS_JOURNAL */
        type = "ext3";
    else
        type = "ext2";

    bpt_emit(out, "ID_FS_TYPE", type);
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    char u[37]; fs_uuid_straight(sb + 104, u); fs_emit_uuid(out, u);
    fs_emit_label(out, sb + 120, 16);
    char ver[16];
    snprintf(ver, sizeof ver, "%u.%u",
             (unsigned)bpt_le32(sb + 0x4C), (unsigned)bpt_le16(sb + 0x7E));
    bpt_emit(out, "ID_FS_VERSION", ver);
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_ext OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add blkid_fs.h tests/test_blkid_fs.c
git commit -m "feat(blkid_fs): ext2/3/4 prober (uuid/label/version/feature-type)"
```

---

### Task 3: btrfs prober

**Files:**
- Modify: `blkid_fs.h`
- Test: `tests/test_blkid_fs.c`

**Interfaces:**
- Consumes: `bpt_read_at`, `fs_uuid_straight`, `fs_emit_uuid`, `fs_emit_label`, `bpt_all_zero`, `bpt_emit`.
- Produces: `int fs_probe_btrfs(const char *dev, struct uevent *out)` — magic `_BHRfS_M` at sb+64 (sb@0x10000); emits TYPE=btrfs, USAGE, UUID (fsid @sb+32), UUID_SUB (dev_item.uuid @sb+267, if non-zero), LABEL (@sb+299, 256B); 0/-1.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_blkid_fs.c`; call `test_btrfs();`:

```c
static void test_btrfs(void) {
    char img[] = "/tmp/fsbtrXXXXXX"; int fd = mkstemp(img); assert(fd >= 0); close(fd);
    zero_img(img, 0x10000 + 1024);
    unsigned char sb[576] = {0};
    memcpy(sb + 64, "_BHRfS_M", 8);
    unsigned char fsid[16] = {0x90,0x55,0x7b,0xe5,0x57,0xa8,0x4f,0xf5,
                              0xbc,0x32,0xe1,0xbc,0x83,0xbe,0x6d,0x75};
    unsigned char sub[16]  = {0x94,0xec,0xd0,0xf5,0x5b,0x70,0x41,0x3e,
                              0xb7,0x1f,0xdd,0x67,0x60,0x66,0x8f,0x32};
    memcpy(sb + 32, fsid, 16);
    memcpy(sb + 267, sub, 16);
    memcpy(sb + 299, "fedora", 6);
    wr_img(img, 0x10000, sb, sizeof sb);

    struct uevent e; e.n = 0;
    assert(fs_probe_btrfs(img, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "btrfs"));
    assert(fs_has(&e, "ID_FS_UUID", "90557be5-57a8-4ff5-bc32-e1bc83be6d75"));
    assert(fs_has(&e, "ID_FS_UUID_SUB", "94ecd0f5-5b70-413e-b71f-dd6760668f32"));
    assert(fs_has(&e, "ID_FS_UUID_SUB_ENC", "94ecd0f5-5b70-413e-b71f-dd6760668f32"));
    assert(fs_has(&e, "ID_FS_LABEL", "fedora"));
    assert(fs_absent(&e, "ID_FS_VERSION"));

    unlink(img);
    printf("test_btrfs OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: FAIL — `fs_probe_btrfs` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `blkid_fs.h` before the `#endif`:

```c
static inline int fs_probe_btrfs(const char *dev, struct uevent *out) {
    unsigned char sb[576];
    if (bpt_read_at(dev, 0x10000, sb, sizeof sb) != 0) return -1;
    if (memcmp(sb + 64, "_BHRfS_M", 8) != 0) return -1;

    bpt_emit(out, "ID_FS_TYPE", "btrfs");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    char u[37]; fs_uuid_straight(sb + 32, u); fs_emit_uuid(out, u);
    if (!bpt_all_zero(sb + 267, 16)) {
        char s[37]; fs_uuid_straight(sb + 267, s);
        bpt_emit(out, "ID_FS_UUID_SUB", s);
        bpt_emit(out, "ID_FS_UUID_SUB_ENC", s);
    }
    fs_emit_label(out, sb + 299, 256);
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_btrfs OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add blkid_fs.h tests/test_blkid_fs.c
git commit -m "feat(blkid_fs): btrfs prober (fsid/sub-uuid/label)"
```

---

### Task 4: vfat + swap probers

**Files:**
- Modify: `blkid_fs.h`
- Test: `tests/test_blkid_fs.c`

**Interfaces:**
- Consumes: `bpt_read_at`, `bpt_le16/32`, `fs_emit_uuid`, `fs_emit_label`, `fs_uuid_straight`, `bpt_all_zero`, `bpt_emit`.
- Produces:
  - `int fs_probe_vfat(const char *dev, struct uevent *out)` — `0x55AA`@510 + BPB sanity, reject NTFS/EXFAT OEM; FAT32 serial@67→`%02X%02X-%02X%02X`, label@71 (`NO NAME    ` ⇒ none), VERSION `FAT32`/`FAT16`; 0/-1.
  - `int fs_probe_swap(const char *dev, struct uevent *out)` — magic `SWAPSPACE2`/`SWAP-SPACE` at pagesize−10 (try common page sizes); UUID straight@1036, LABEL@1052, VERSION `%u`@1024, USAGE=other; 0/-1.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_blkid_fs.c`; call `test_vfat_swap();`:

```c
static void test_vfat_swap(void) {
    /* vfat FAT32: serial 7c 76 73 07 -> 0773-767C; label "NO NAME    " -> none */
    char v[] = "/tmp/fsvfatXXXXXX"; int vf = mkstemp(v); assert(vf >= 0); close(vf);
    zero_img(v, 512);
    unsigned char bs[512] = {0};
    bs[11] = 0x00; bs[12] = 0x02;   /* bytes/sector = 512 */
    bs[13] = 8;                     /* sec/cluster */
    /* fatsz16 (@22) = 0 -> FAT32 */
    bs[67] = 0x7c; bs[68] = 0x76; bs[69] = 0x73; bs[70] = 0x07;      /* serial */
    memcpy(bs + 71, "NO NAME    ", 11);                              /* no label */
    bs[510] = 0x55; bs[511] = 0xaa;
    wr_img(v, 0, bs, sizeof bs);

    struct uevent e; e.n = 0;
    assert(fs_probe_vfat(v, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "vfat"));
    assert(fs_has(&e, "ID_FS_UUID", "0773-767C"));
    assert(fs_has(&e, "ID_FS_VERSION", "FAT32"));
    assert(fs_absent(&e, "ID_FS_LABEL"));
    unlink(v);

    /* labeled FAT32 */
    char v2[] = "/tmp/fsvfat2XXXXXX"; int vf2 = mkstemp(v2); assert(vf2 >= 0); close(vf2);
    zero_img(v2, 512);
    bs[13] = 8; memcpy(bs + 71, "MYSTICK    ", 11);
    wr_img(v2, 0, bs, sizeof bs);
    struct uevent e2; e2.n = 0;
    assert(fs_probe_vfat(v2, &e2) == 0);
    assert(fs_has(&e2, "ID_FS_LABEL", "MYSTICK"));
    unlink(v2);

    /* swap: version 1, uuid c2e50da1..., usage other, page 4096 */
    char s[] = "/tmp/fsswapXXXXXX"; int sf = mkstemp(s); assert(sf >= 0); close(sf);
    zero_img(s, 8192);
    unsigned char hdr[64] = {0};
    hdr[0] = 1;                     /* version @1024 = 1 */
    unsigned char su[16] = {0xc2,0xe5,0x0d,0xa1,0x0f,0x93,0x4f,0x2b,
                            0x81,0x32,0x29,0xe3,0x14,0xf2,0xc8,0x27};
    memcpy(hdr + 12, su, 16);       /* uuid @1036 */
    wr_img(s, 1024, hdr, sizeof hdr);
    unsigned char mg[10]; memcpy(mg, "SWAPSPACE2", 10);
    wr_img(s, 4096 - 10, mg, 10);
    struct uevent e3; e3.n = 0;
    assert(fs_probe_swap(s, &e3) == 0);
    assert(fs_has(&e3, "ID_FS_TYPE", "swap"));
    assert(fs_has(&e3, "ID_FS_USAGE", "other"));
    assert(fs_has(&e3, "ID_FS_UUID", "c2e50da1-0f93-4f2b-8132-29e314f2c827"));
    assert(fs_has(&e3, "ID_FS_VERSION", "1"));
    unlink(s);

    printf("test_vfat_swap OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: FAIL — `fs_probe_vfat` / `fs_probe_swap` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `blkid_fs.h` before the `#endif`:

```c
static inline int fs_probe_vfat(const char *dev, struct uevent *out) {
    unsigned char bs[512];
    if (bpt_read_at(dev, 0, bs, sizeof bs) != 0) return -1;
    if (!(bs[510] == 0x55 && bs[511] == 0xaa)) return -1;
    if (memcmp(bs + 3, "NTFS", 4) == 0 || memcmp(bs + 3, "EXFAT", 5) == 0) return -1;
    unsigned bps = bpt_le16(bs + 11);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || bs[13] == 0) return -1;

    int is_fat32 = (bpt_le16(bs + 22) == 0);
    unsigned ser = is_fat32 ? 67 : 39;
    unsigned lbo = is_fat32 ? 71 : 43;

    bpt_emit(out, "ID_FS_TYPE", "vfat");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    char u[16];
    snprintf(u, sizeof u, "%02X%02X-%02X%02X", bs[ser + 3], bs[ser + 2], bs[ser + 1], bs[ser]);
    fs_emit_uuid(out, u);
    if (memcmp(bs + lbo, "NO NAME    ", 11) != 0) {
        unsigned char lbl[12]; memcpy(lbl, bs + lbo, 11); lbl[11] = '\0';
        int end = 11; while (end > 0 && lbl[end - 1] == ' ') end--;
        lbl[end] = '\0';
        if (end > 0) fs_emit_label(out, lbl, (size_t)end);
    }
    bpt_emit(out, "ID_FS_VERSION", is_fat32 ? "FAT32" : "FAT16");   /* FAT12 vs 16: see note */
    return 0;
}

static inline int fs_probe_swap(const char *dev, struct uevent *out) {
    static const uint64_t pages[] = {4096, 65536, 16384, 8192, 2048, 1024};
    int found = 0;
    for (unsigned i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        unsigned char m[10];
        if (bpt_read_at(dev, pages[i] - 10, m, 10) != 0) continue;
        if (memcmp(m, "SWAPSPACE2", 10) == 0 || memcmp(m, "SWAP-SPACE", 10) == 0) { found = 1; break; }
    }
    if (!found) return -1;
    unsigned char hdr[64];
    if (bpt_read_at(dev, 1024, hdr, sizeof hdr) != 0) return -1;

    bpt_emit(out, "ID_FS_TYPE", "swap");
    bpt_emit(out, "ID_FS_USAGE", "other");
    if (!bpt_all_zero(hdr + 12, 16)) {
        char u[37]; fs_uuid_straight(hdr + 12, u); fs_emit_uuid(out, u);
    }
    fs_emit_label(out, hdr + 28, 16);   /* volume_name @ 1024+28 = 1052 */
    char ver[16]; snprintf(ver, sizeof ver, "%u", (unsigned)bpt_le32(hdr));
    bpt_emit(out, "ID_FS_VERSION", ver);
    return 0;
}
```

Note (source-parity): FAT12 vs FAT16 requires the cluster-count computation from libblkid `superblocks/vfat.c`; both blakbox vfat volumes are FAT32, so the FAT16/12 branch is unit-tested only — reconcile the exact FAT12/16 threshold toward the source if a real small-FAT volume ever appears.

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_vfat_swap OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add blkid_fs.h tests/test_blkid_fs.c
git commit -m "feat(blkid_fs): vfat + swap probers"
```

---

### Task 5: ntfs (MFT walk) + exfat probers

**Files:**
- Modify: `blkid_fs.h`
- Test: `tests/test_blkid_fs.c`

**Interfaces:**
- Consumes: `bpt_read_at`, `bpt_le16/32/64`, `fs_emit_uuid`, `fs_emit_label`, `fs_utf16_to_utf8`, `bpt_emit`.
- Produces:
  - `int fs_probe_ntfs(const char *dev, struct uevent *out)` — magic `NTFS    `@3; UUID = serial@72 reversed 16-hex UPPER; LABEL via MFT `$Volume` (record 3) `$VOLUME_NAME` (0x60) walk; 0/-1.
  - `int fs_probe_exfat(const char *dev, struct uevent *out)` — magic `EXFAT   `@3; UUID = serial@100 → `%04X-%04X`; TYPE=exfat, USAGE=filesystem (LABEL via root-dir entry is deferred/source-ported — no exFAT here). 0/-1.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_blkid_fs.c`; call `test_ntfs_exfat();`. This builds a synthetic NTFS with a one-record MFT holding a `$VOLUME_NAME` = `RECOVERY`:

```c
static void test_ntfs_exfat(void) {
    /* NTFS: bps=512, spc=8 -> cluster 4096; mft_lcn=1 -> mft @4096; rec_desc=-10 -> rec 1024.
       $Volume is record 3 -> mft@4096 + 3*1024 = 7168. Serial @72 (8B) reversed. */
    char n[] = "/tmp/fsntfsXXXXXX"; int nf = mkstemp(n); assert(nf >= 0); close(nf);
    zero_img(n, 16384);
    unsigned char bs[512] = {0};
    memcpy(bs + 3, "NTFS    ", 8);
    bs[11] = 0x00; bs[12] = 0x02;   /* bytes/sector = 512 */
    bs[13] = 8;                     /* sec/cluster -> cluster 4096 */
    bs[48] = 1;                     /* mft_lcn = 1 (u64 LE) */
    bs[64] = (unsigned char)(-10);  /* clusters_per_mft_record = -10 -> 1<<10 = 1024 */
    /* serial @72: bytes -> printed reversed as 6E54847B54844833 */
    unsigned char ser[8] = {0x33,0x48,0x84,0x54,0x7b,0x84,0x54,0x6e};
    memcpy(bs + 72, ser, 8);
    bs[510] = 0x55; bs[511] = 0xaa;
    wr_img(n, 0, bs, sizeof bs);

    /* MFT record 3 @ 7168: "FILE" header, usa_off=48 usa_cnt=3, first_attr@56,
       one $VOLUME_NAME (0x60) resident attr with value "RECOVERY" (UTF-16LE). */
    unsigned char rec[1024] = {0};
    memcpy(rec, "FILE", 4);
    rec[4] = 48; rec[5] = 0;        /* usa_offset = 48 */
    rec[6] = 3;  rec[7] = 0;        /* usa_count = 3 */
    rec[20] = 56; rec[21] = 0;      /* first attribute offset = 56 */
    /* attribute @56: type 0x60, len 40, non-res=0, name_len=0, value_len, value_off */
    unsigned char *a = rec + 56;
    a[0] = 0x60;                    /* type $VOLUME_NAME */
    a[4] = 40;                      /* attr length = 40 */
    a[16] = 16; a[17] = 0;          /* value_length = 16 (8 UTF-16 chars) */
    a[20] = 24; a[21] = 0;          /* value_offset = 24 */
    const char *L = "RECOVERY";
    for (int i = 0; L[i]; i++) a[24 + i*2] = (unsigned char)L[i];
    /* terminator after the attr */
    rec[56 + 40] = 0xff; rec[56 + 41] = 0xff; rec[56 + 42] = 0xff; rec[56 + 43] = 0xff;
    /* fixup: usa[0]=signature word; sectors' last 2 bytes must equal usa[0] then get restored.
       Keep signature 0 so the fixup writes zeros (harmless for our short attr region). */
    wr_img(n, 7168, rec, sizeof rec);

    struct uevent e; e.n = 0;
    assert(fs_probe_ntfs(n, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "ntfs"));
    assert(fs_has(&e, "ID_FS_UUID", "6E54847B54844833"));
    assert(fs_has(&e, "ID_FS_LABEL", "RECOVERY"));
    unlink(n);

    /* exfat: serial @100 = 0x12345678 -> "1234-5678" */
    char x[] = "/tmp/fsexfatXXXXXX"; int xf = mkstemp(x); assert(xf >= 0); close(xf);
    zero_img(x, 512);
    unsigned char xb[512] = {0};
    memcpy(xb + 3, "EXFAT   ", 8);
    xb[100] = 0x78; xb[101] = 0x56; xb[102] = 0x34; xb[103] = 0x12;   /* serial LE */
    wr_img(x, 0, xb, sizeof xb);
    struct uevent e2; e2.n = 0;
    assert(fs_probe_exfat(x, &e2) == 0);
    assert(fs_has(&e2, "ID_FS_TYPE", "exfat"));
    assert(fs_has(&e2, "ID_FS_UUID", "1234-5678"));
    unlink(x);

    printf("test_ntfs_exfat OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: FAIL — `fs_probe_ntfs` / `fs_probe_exfat` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `blkid_fs.h` before the `#endif`:

```c
static inline int fs_probe_ntfs(const char *dev, struct uevent *out) {
    unsigned char bs[512];
    if (bpt_read_at(dev, 0, bs, sizeof bs) != 0) return -1;
    if (memcmp(bs + 3, "NTFS    ", 8) != 0) return -1;

    bpt_emit(out, "ID_FS_TYPE", "ntfs");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    char u[17];
    snprintf(u, sizeof u, "%02X%02X%02X%02X%02X%02X%02X%02X",
             bs[79], bs[78], bs[77], bs[76], bs[75], bs[74], bs[73], bs[72]);
    fs_emit_uuid(out, u);

    unsigned bps = bpt_le16(bs + 11);
    unsigned spc = bs[13];
    if (bps == 0 || spc == 0) return 0;
    uint64_t cluster = (uint64_t)bps * spc;
    int64_t mft_lcn = (int64_t)bpt_le64(bs + 48);
    int8_t rd = (int8_t)bs[64];
    uint64_t rec_size = (rd < 0) ? ((uint64_t)1 << (unsigned)(-rd)) : (uint64_t)rd * cluster;
    if (mft_lcn <= 0 || rec_size < 42 || rec_size > 65536) return 0;

    unsigned char *rec = (unsigned char *)malloc(rec_size);
    if (!rec) return 0;
    if (bpt_read_at(dev, (uint64_t)mft_lcn * cluster + 3 * rec_size, rec, rec_size) != 0 ||
        memcmp(rec, "FILE", 4) != 0) { free(rec); return 0; }

    /* apply the update-sequence-array fixup */
    unsigned usa_off = bpt_le16(rec + 4), usa_cnt = bpt_le16(rec + 6);
    if (usa_off && usa_cnt > 1 && bps) {
        for (unsigned i = 1; i < usa_cnt; i++) {
            uint64_t pos = (uint64_t)i * bps - 2;
            if (pos + 2 <= rec_size && usa_off + i * 2 + 1 < rec_size) {
                rec[pos]     = rec[usa_off + i * 2];
                rec[pos + 1] = rec[usa_off + i * 2 + 1];
            }
        }
    }

    unsigned ao = bpt_le16(rec + 20);
    while (ao + 8 <= rec_size) {
        uint32_t atype = bpt_le32(rec + ao);
        if (atype == 0xffffffffu) break;
        uint32_t alen = bpt_le32(rec + ao + 4);
        if (alen == 0 || (uint64_t)ao + alen > rec_size) break;
        if (atype == 0x60) {   /* $VOLUME_NAME, resident */
            uint32_t vlen = bpt_le32(rec + ao + 16);
            uint32_t voff = bpt_le16(rec + ao + 20);
            if (vlen > 0 && (uint64_t)ao + voff + vlen <= rec_size) {
                char lbl[256];
                fs_utf16_to_utf8(rec + ao + voff, vlen, lbl, sizeof lbl);
                fs_emit_label(out, (unsigned char *)lbl, strlen(lbl));
            }
            break;
        }
        ao += alen;
    }
    free(rec);
    return 0;
}

static inline int fs_probe_exfat(const char *dev, struct uevent *out) {
    unsigned char vbr[512];
    if (bpt_read_at(dev, 0, vbr, sizeof vbr) != 0) return -1;
    if (memcmp(vbr + 3, "EXFAT   ", 8) != 0) return -1;

    bpt_emit(out, "ID_FS_TYPE", "exfat");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    uint32_t serial = bpt_le32(vbr + 100);
    char u[16];
    snprintf(u, sizeof u, "%04X-%04X", (unsigned)(serial >> 16), (unsigned)(serial & 0xffff));
    fs_emit_uuid(out, u);
    /* exFAT LABEL lives in a root-directory volume-label entry (cluster walk) — deferred;
       port from libblkid superblocks/exfat.c if a real exFAT volume ever needs it. */
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_ntfs_exfat OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add blkid_fs.h tests/test_blkid_fs.c
git commit -m "feat(blkid_fs): ntfs (MFT label walk) + exfat probers"
```

---

### Task 6: orchestrator `blkid_fs_build`

**Files:**
- Modify: `blkid_fs.h`
- Test: `tests/test_blkid_fs.c`

**Interfaces:**
- Consumes: all six probers.
- Produces: `int blkid_fs_build(const char *sysroot, const char *devpath, const char *devnode, struct uevent *out)` — tries the probers in order btrfs → ext → ntfs → exfat → vfat → swap; first match emits and returns; no match emits nothing. Returns 0 always. `sysroot`/`devpath` unused (signature symmetry).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_blkid_fs.c`; call `test_build();`:

```c
static void test_build(void) {
    /* reuse the ext4 image path: build a real ext4 sb, drive the orchestrator */
    char img[] = "/tmp/fsbuildXXXXXX"; int fd = mkstemp(img); assert(fd >= 0); close(fd);
    zero_img(img, 4096);
    unsigned char sb[264] = {0};
    sb[56] = 0x53; sb[57] = 0xef;
    unsigned char uu[16] = {0xcf,0x4f,0x2b,0x07,0xf1,0x50,0x40,0x4f,
                            0xbd,0xe1,0x4d,0x9f,0x54,0x55,0x94,0xb4};
    memcpy(sb + 104, uu, 16); sb[0x4C] = 1; sb[0x60] = 0x40;
    wr_img(img, 1024, sb, sizeof sb);

    struct uevent e;
    assert(blkid_fs_build("/sys", "/devices/x", img, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "ext4"));
    assert(fs_has(&e, "ID_FS_UUID", "cf4f2b07-f150-404f-bde1-4d9f545594b4"));
    unlink(img);

    /* unformatted device -> nothing */
    char z[] = "/tmp/fszeroXXXXXX"; int zf = mkstemp(z); assert(zf >= 0); close(zf);
    zero_img(z, 65536 + 1024);
    struct uevent e2;
    assert(blkid_fs_build("/sys", "/devices/z", z, &e2) == 0);
    assert(e2.n == 0);
    unlink(z);

    printf("test_build OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_blkid_fs.c -o /tmp/t && /tmp/t`
Expected: FAIL — `blkid_fs_build` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `blkid_fs.h` before the `#endif`:

```c
static inline int blkid_fs_build(const char *sysroot, const char *devpath,
                                 const char *devnode, struct uevent *out) {
    (void)sysroot; (void)devpath;
    out->n = 0;
    if (fs_probe_btrfs(devnode, out) == 0) return 0;
    if (fs_probe_ext(devnode, out)   == 0) return 0;
    if (fs_probe_ntfs(devnode, out)  == 0) return 0;
    if (fs_probe_exfat(devnode, out) == 0) return 0;
    if (fs_probe_vfat(devnode, out)  == 0) return 0;
    if (fs_probe_swap(devnode, out)  == 0) return 0;
    out->n = 0;
    return 0;
}
```

- [ ] **Step 4: Run the full suite**

Run: `make test`
Expected: PASS — all prior suites plus `test_helpers` / `test_ext` / `test_btrfs` / `test_vfat_swap` / `test_ntfs_exfat` / `test_build` / `ALL blkid_fs tests passed`. No warnings.

- [ ] **Step 5: Verify the boundary**

Run: `git diff origin/master -- schema-udev.c schema-udev.h; grep -c blkid_fs schema-udev.c`
Expected: empty diff, `0`.

- [ ] **Step 6: Commit**

```bash
git add blkid_fs.h tests/test_blkid_fs.c
git commit -m "feat(blkid_fs): orchestrator blkid_fs_build + probe dispatch"
```

---

### Task 7: live parity harness (sudo) + vmtest

**Files:**
- Create: `tests/verify_blkid_fs_live.sh`

**Interfaces:**
- Consumes: `blkid_fs_build` from `blkid_fs.h`.
- Produces: an executable acceptance script; prints `blkid_fs live parity: N devices, M mismatches` and exits non-zero if M > 0.

- [ ] **Step 1: Write the harness**

Create `tests/verify_blkid_fs_live.sh`:

```sh
#!/bin/sh
# Live parity gate: run blkid_fs_build over every /sys/class/block node, diff the
# identity ID_FS_* subset vs `udevadm info` BOTH directions. SIZE/BLOCKSIZE/LASTBLOCK
# are deferred (excluded both sides). Reads raw devices -> runs under sudo.
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/blkidfs_driver.c <<'EOF'
#include "blkid_fs.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;                 /* argv[1]=devpath argv[2]=devnode */
    struct uevent ev;
    if (blkid_fs_build("/sys", argv[1], argv[2], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/blkidfs_driver.c -o /tmp/blkidfs_driver

# identity fields only; exclude the deferred size trio
KEYS='^ID_FS_(TYPE|USAGE|UUID|UUID_ENC|LABEL|LABEL_ENC|UUID_SUB|UUID_SUB_ENC|VERSION)='
DEFER='^ID_FS_(SIZE|BLOCKSIZE|LASTBLOCK)='

props=$(mktemp)
fsprops=$(mktemp)
misses=$(mktemp)
total=0
for blk in /sys/class/block/*; do
    [ -e "$blk" ] || continue
    name=$(basename "$blk")
    dev=$(readlink -f "$blk"); devpath=${dev#/sys}
    devnode="/dev/$name"
    [ -b "$devnode" ] || continue
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -E "$KEYS" "$props" | grep -vE "$DEFER" > "$fsprops" || true
    emitted=$(sudo /tmp/blkidfs_driver "$devpath" "$devnode")
    [ -n "$emitted" ] || [ -s "$fsprops" ] || continue
    total=$((total + 1))
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$fsprops" || printf 'MISMATCH(val) %s | emit=%s\n' "$name" "$line"
    done >> "$misses"
    while IFS= read -r uline; do
        [ -n "$uline" ] || continue
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$name" "$uline"
    done < "$fsprops" >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'blkid_fs live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$fsprops" "$misses"
[ "$miss" -eq 0 ]
```

- [ ] **Step 2: Run the live gate**

Run: `chmod +x tests/verify_blkid_fs_live.sh && tests/verify_blkid_fs_live.sh`
Expected: `blkid_fs live parity: 11 devices, 0 mismatches` and exit 0. (10 partition filesystems + swap zram0; count varies with what's attached — only `0 mismatches` gates.)

If any `MISMATCH` prints: the emitted line and udev's value are shown. Fix the prober in `blkid_fs.h` (consult libblkid), re-run. Do NOT touch `schema-udev.c`.

- [ ] **Step 3: Run the vmtest boot rail**

Run: `cd ~/schema-livetest && ./vmtest.sh` then `cd -`
Expected: `>> RESULT: PASS`.

- [ ] **Step 4: Commit**

```bash
git add tests/verify_blkid_fs_live.sh
git commit -m "test(blkid_fs): live udev parity acceptance harness (sudo, identity subset)"
```

- [ ] **Step 5: Push and confirm origin == local (hard rule)**

```bash
git push -u origin feat/schema-udev-blkid-fs
git ls-remote origin refs/heads/feat/schema-udev-blkid-fs
git rev-parse HEAD
```

Expected: the two hashes are identical. Only then is the work landable.

---

## Notes for the implementer (Greg)

- **All offsets are verified against blakbox raw disks.** ext4 UUID straight @sb+104; btrfs fsid@sb+32 / sub@sb+267 / label@sb+299; vfat FAT32 serial@67 (`NO NAME    ` = no label); ntfs serial@72 printed **byte-reversed** uppercase; swap uuid@1036 / version@1024 / USAGE=`other`. Do not "fix" the straight-byte UUID order to PR-A's mixed-endian — FS UUIDs are straight.
- **NTFS label is the hard one** — a real MFT walk (boot sector → cluster & MFT-record sizes → record 3 → fixup → `$VOLUME_NAME` attr). sdb4=`RECOVERY`, sdb2=no label are the live vectors.
- **First-match-wins order matters:** ntfs/exfat before vfat (all three carry `0x55AA`); the offset-3 OEM magic disambiguates.
- **Deferred trio:** never emit `ID_FS_SIZE`/`ID_FS_BLOCKSIZE`/`ID_FS_LASTBLOCK`. The live gate filters them out both directions; emitting one shows as an over-emission MISMATCH.
- **exFAT/FAT12-16 branches are unit-tested only** (no such media here); port exact bytes from libblkid, source governs.
- `schema-udev.c`/`.h` are frozen. If a test seems to need a change there, stop — it doesn't.
- The live gate is the final authority (needs `sudo`). If libblkid and this plan ever differ, follow the source and let the gate confirm.
- This is **PR-B** of blkid; PR-A (partition tables) already merged. After I (Claire) verify all gates, the branch lands via PR. Do not open the PR yourself.
```
