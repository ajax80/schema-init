# schema-udev cdrom_id capabilities (sub-project B slice 3d) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reimplement udev's `cdrom_id` drive-capability detection as `cdrom_id.h`, wired into `run_builtins`, so optical drives get their `ID_CDROM_*` capability keys natively via MMC GET CONFIGURATION; bring those keys in-scope in the parity classifier while deferring media-status keys to slice 3e.

**Architecture:** New header-only `cdrom_id.h` (ata_id pattern: a pure `cdrom_id_decode` split from the SG_IO GET CONFIGURATION ioctl for unit-testing). Dispatched via a new `UB_CDROM` gate on optical block devices (`sr*`/`scd*`). The classifier maps `ID_CDROM*` to the `cdrom_id` builtin, makes capability keys in-scope, and defers `ID_CDROM_MEDIA_*`.

**Tech Stack:** C (C99, `-Wall -Wextra`), Linux SG_IO (`<scsi/sg.h>`), existing schema-init Makefile + test harness, `~/schema-livetest/vmtest.sh`.

## Global Constraints

- **Capabilities only.** Emit `ID_CDROM=1` + the profile-derived `ID_CDROM_*` keys. Do NOT emit any `ID_CDROM_MEDIA_*` key — that is slice 3e (needs a disc). `ID_CDROM_MEDIA_*` is documented-deferred in the classifier.
- **Ground truth (sr0, empty drive):** the 384-byte GET CONFIGURATION fixture's 14 profiles must yield exactly these 17 keys, each `"1"`: `ID_CDROM`, `ID_CDROM_CD`, `ID_CDROM_CD_R`, `ID_CDROM_CD_RW`, `ID_CDROM_DVD`, `ID_CDROM_DVD_R`, `ID_CDROM_DVD_RAM`, `ID_CDROM_DVD_RW`, `ID_CDROM_DVD_RW_RO`, `ID_CDROM_DVD_RW_SEQ`, `ID_CDROM_DVD_R_DL`, `ID_CDROM_DVD_R_DL_SEQ`, `ID_CDROM_DVD_R_DL_JR`, `ID_CDROM_DVD_PLUS_R`, `ID_CDROM_DVD_PLUS_RW`, `ID_CDROM_DVD_PLUS_R_DL`, `ID_CDROM_RW_REMOVABLE`.
- **`ID_CDROM_RW_REMOVABLE`** = profile `0x02` (Removable disk). Resolved from the fixture (the Profile List alone yields all 17; no separate feature parse needed).
- **Boundary:** `schema-udev.c`, `schema-udev.h`, and the group-1 netlink bind stay byte-identical. Changes: `cdrom_id.h` (new), `udev_builtins.h`, `udev-parity.h`, `tests/`, `Makefile`.
- **Honesty:** the fixture unit test asserts EXACTLY the 17 keys (no more, no fewer, no media keys); the live gate asserts the parity tool's computed counters + positive reproduction.
- Terse code, style matches surrounding files.

## Fixture

`tests/fixtures/cdrom_getconf_sr0.h` is committed: `static const uint8_t cdrom_getconf_sr0[384]` (real empty-drive GET CONFIGURATION) with the 17 ground-truth keys in its header comment.

---

### Task 1: `cdrom_id.h` (GET CONFIGURATION + pure decode) + unit tests

**Files:**
- Create: `cdrom_id.h`
- Test: `tests/test_cdrom_id.c`
- Modify: `Makefile` (add `test_cdrom_id` to the `test` target)

**Interfaces:**
- Produces: `cdrom_get_config(const char *devnode, uint8_t *buf, size_t bufsz, int *len) -> int`; `cdrom_id_decode(const uint8_t *buf, int len, struct uevent *out) -> int`; `cdrom_id_build(sysroot, devpath, devnode, out) -> int`.

- [ ] **Step 1: Write the failing unit test**

Create `tests/test_cdrom_id.c`:

```c
#include "../cdrom_id.h"
#include "fixtures/cdrom_getconf_sr0.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int has(const struct uevent *ev, const char *k) {
    const char *v = uevent_get(ev, k);
    return v && strcmp(v, "1") == 0;
}

int main(void) {
    struct uevent ev;
    int n = cdrom_id_decode(cdrom_getconf_sr0, sizeof cdrom_getconf_sr0, &ev);

    const char *want[] = {
        "ID_CDROM", "ID_CDROM_CD", "ID_CDROM_CD_R", "ID_CDROM_CD_RW",
        "ID_CDROM_DVD", "ID_CDROM_DVD_R", "ID_CDROM_DVD_RAM",
        "ID_CDROM_DVD_RW", "ID_CDROM_DVD_RW_RO", "ID_CDROM_DVD_RW_SEQ",
        "ID_CDROM_DVD_R_DL", "ID_CDROM_DVD_R_DL_SEQ", "ID_CDROM_DVD_R_DL_JR",
        "ID_CDROM_DVD_PLUS_R", "ID_CDROM_DVD_PLUS_RW", "ID_CDROM_DVD_PLUS_R_DL",
        "ID_CDROM_RW_REMOVABLE", NULL
    };
    int nwant = 0;
    for (int i = 0; want[i]; i++) { assert(has(&ev, want[i])); nwant++; }
    assert(nwant == 17);
    assert(n == 17);                 /* exactly 17 — no extras */
    /* no media keys this slice */
    for (int i = 0; i < ev.n; i++) assert(strncmp(ev.key[i], "ID_CDROM_MEDIA", 14) != 0);

    /* truncated blob -> 0 keys, no crash */
    struct uevent ev2;
    assert(cdrom_id_decode(cdrom_getconf_sr0, 4, &ev2) == 0);

    /* synthetic single-profile (0x08 CD-ROM): header(8) + profile-list feature */
    uint8_t b[16] = {0,0,0,12, 0,0,0,0,   /* header: datalen 12, current profile 0 */
                     0x00,0x00,0x03,0x04, /* feature 0x0000, addlen 4 */
                     0x00,0x08,0x00,0x00}; /* profile 0x08 */
    struct uevent ev3;
    cdrom_id_decode(b, sizeof b, &ev3);
    assert(has(&ev3, "ID_CDROM") && has(&ev3, "ID_CDROM_CD"));
    assert(!has(&ev3, "ID_CDROM_DVD"));

    printf("test_cdrom_id: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_cdrom_id.c -o /tmp/schema-test-cdromid && /tmp/schema-test-cdromid`
Expected: FAIL — compile error (`cdrom_id.h` / `cdrom_id_decode` do not exist).

- [ ] **Step 3: Implement `cdrom_id.h`**

Create `cdrom_id.h`:

```c
#ifndef CDROM_ID_H
#define CDROM_ID_H

#include "schema-udev.h"
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static inline int cdrom_id_decode(const uint8_t *buf, int len, struct uevent *out) {
    out->n = 0;
    if (len < 8) return 0;
    #define CEMIT(k) do { \
        if (out->n < UE_MAX_KEYS && !uevent_get(out, (k))) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], "1", UE_VAL_MAX); \
            out->n++; \
        } \
    } while (0)

    int off = 8;                                  /* skip 8-byte header */
    while (off + 4 <= len) {
        unsigned feat = ((unsigned)buf[off] << 8) | buf[off + 1];
        int addlen = buf[off + 3];
        if (off + 4 + addlen > len) break;
        if (feat == 0x0000) {                     /* Profile List */
            CEMIT("ID_CDROM");
            const uint8_t *fd = buf + off + 4;
            for (int p = 0; p + 4 <= addlen; p += 4) {
                unsigned profile = ((unsigned)fd[p] << 8) | fd[p + 1];
                switch (profile) {
                case 0x02: CEMIT("ID_CDROM_RW_REMOVABLE"); break;
                case 0x08: CEMIT("ID_CDROM_CD"); break;
                case 0x09: CEMIT("ID_CDROM_CD_R"); break;
                case 0x0a: CEMIT("ID_CDROM_CD_RW"); break;
                case 0x10: CEMIT("ID_CDROM_DVD"); break;
                case 0x11: CEMIT("ID_CDROM_DVD_R"); break;
                case 0x12: CEMIT("ID_CDROM_DVD_RAM"); break;
                case 0x13: CEMIT("ID_CDROM_DVD_RW"); CEMIT("ID_CDROM_DVD_RW_RO"); break;
                case 0x14: CEMIT("ID_CDROM_DVD_RW"); CEMIT("ID_CDROM_DVD_RW_SEQ"); break;
                case 0x15: CEMIT("ID_CDROM_DVD_R_DL"); CEMIT("ID_CDROM_DVD_R_DL_SEQ"); break;
                case 0x16: CEMIT("ID_CDROM_DVD_R_DL"); CEMIT("ID_CDROM_DVD_R_DL_JR"); break;
                case 0x1a: CEMIT("ID_CDROM_DVD_PLUS_RW"); break;
                case 0x1b: CEMIT("ID_CDROM_DVD_PLUS_R"); break;
                case 0x2a: CEMIT("ID_CDROM_DVD_PLUS_RW_DL"); break;
                case 0x2b: CEMIT("ID_CDROM_DVD_PLUS_R_DL"); break;
                case 0x40: CEMIT("ID_CDROM_BD"); break;
                case 0x41: case 0x42: CEMIT("ID_CDROM_BD_R"); break;
                case 0x43: CEMIT("ID_CDROM_BD_RE"); break;
                case 0x50: CEMIT("ID_CDROM_HDDVD"); break;
                case 0x51: CEMIT("ID_CDROM_HDDVD_R"); break;
                case 0x52: CEMIT("ID_CDROM_HDDVD_RW"); break;
                default: break;
                }
            }
        }
        off += 4 + addlen;
    }
    #undef CEMIT
    return out->n;
}

static inline int cdrom_get_config(const char *devnode, uint8_t *buf, size_t bufsz, int *len) {
    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    memset(buf, 0, bufsz);
    uint8_t cdb[10] = {0x46, 0x00, 0, 0, 0, 0, 0,
                       (uint8_t)(bufsz >> 8), (uint8_t)(bufsz & 0xff), 0};
    uint8_t sense[32] = {0};
    struct sg_io_hdr io = {0};
    io.interface_id = 'S';
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.cmd_len = sizeof cdb;
    io.cmdp = cdb;
    io.dxfer_len = (unsigned)bufsz;
    io.dxferp = buf;
    io.sbp = sense;
    io.mx_sb_len = sizeof sense;
    io.timeout = 5000;
    int rc = ioctl(fd, SG_IO, &io);
    close(fd);
    if (rc < 0 || (io.info & SG_INFO_OK_MASK) != SG_INFO_OK) return -1;
    int datalen = ((int)buf[0] << 24) | ((int)buf[1] << 16) | ((int)buf[2] << 8) | buf[3];
    int total = datalen + 4;
    if (total > (int)bufsz) total = (int)bufsz;
    *len = total;
    return 0;
}

static inline int cdrom_id_build(const char *sysroot, const char *devpath,
                                 const char *devnode, struct uevent *out) {
    (void)sysroot; (void)devpath;
    out->n = 0;
    if (!devnode) return 0;
    uint8_t buf[2048];
    int len = 0;
    if (cdrom_get_config(devnode, buf, sizeof buf, &len) != 0) return 0;
    return cdrom_id_decode(buf, len, out);
}

#endif /* CDROM_ID_H */
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_cdrom_id.c -o /tmp/schema-test-cdromid && /tmp/schema-test-cdromid`
Expected: PASS — `test_cdrom_id: OK`.

- [ ] **Step 5: Add the test to the Makefile `test` target**

Add after the `test_v4l_id` line:

```make
	$(CC) $(CFLAGS) tests/test_cdrom_id.c -o /tmp/schema-test-cdromid && /tmp/schema-test-cdromid
```

- [ ] **Step 6: Commit**

```bash
git add cdrom_id.h tests/test_cdrom_id.c tests/fixtures/cdrom_getconf_sr0.h Makefile
git commit -m "feat(cdrom_id): GET CONFIGURATION profile-list decode + fixture tests"
```

---

### Task 2: Wire `UB_CDROM` dispatch + parity scope

**Files:**
- Modify: `udev_builtins.h`, `udev-parity.h`

**Interfaces:**
- Consumes: `cdrom_id_build`; produces `UB_CDROM` bit, `parity_cdrom_media`.

- [ ] **Step 1: Add the `UB_CDROM` gate to `udev_builtins.h`**

Add `#include "cdrom_id.h"` with the other builtin includes. Extend the enum:

```c
enum { UB_HWDB = 1, UB_PATH = 2, UB_USB = 4, UB_INPUT = 8, UB_NET = 16, UB_BLKID = 32, UB_ATA = 64, UB_V4L = 128, UB_CDROM = 256 };
```

In `ub_select`, after the `UB_BLKID` block, add:

```c
    if (subsystem && strcmp(subsystem, "block") == 0 &&
        (fnmatch("sr*", kname, 0) == 0 || fnmatch("scd*", kname, 0) == 0))
        sel |= UB_CDROM;
```

- [ ] **Step 2: Dispatch `cdrom_id` in `run_builtins`**

After the `if (sel & UB_BLKID) { ... }` block, add:

```c
    if (sel & UB_CDROM) { tmp.n = 0; cdrom_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); }
```

- [ ] **Step 3: Parity — hint + media deferral + capability in-scope (`udev-parity.h`)**

In `parity_builtin_hint`, add before the `usb_id` catch-all block:

```c
    if (strncmp(key, "ID_CDROM", 8) == 0) return "cdrom_id";
```

Add a media-deferral predicate near `parity_ata_feature`:

```c
static inline int parity_cdrom_media(const char *key) {
    return strncmp(key, "ID_CDROM_MEDIA", 14) == 0;   /* media status -> slice 3e */
}
```

In `parity_in_scope_missing`, in the `block` branch, after the `parity_ata_feature` line add the media deferral, and after the identity-key check add the cdrom capability in-scope rule:

```c
    if (sub && strcmp(sub, "block") == 0) {
        if (parity_ata_feature(key)) return 0;
        if (parity_cdrom_media(key)) return 0;                 /* slice-3e deferral */
        if (strcmp(key, "ID_PATH") == 0 || strcmp(key, "ID_PATH_TAG") == 0 ||
            strstr(key, "_FROM_DATABASE") != NULL ||
            strncmp(key, "ID_FS_", 6) == 0 || strncmp(key, "ID_PART_", 8) == 0 ||
            strcmp(key, "ID_USB_INTERFACE_NUM") == 0 || strcmp(key, "ID_USB_DRIVER") == 0)
            return 1;
        if (udev_identity_key(key) && devpath &&
            (strstr(devpath, "/ata") != NULL || strstr(devpath, "/usb") != NULL))
            return 1;
        if (strncmp(key, "ID_CDROM", 8) == 0) return 1;        /* cdrom capabilities (3d) */
        return 0;
    }
```

- [ ] **Step 4: Build daemon + parity tool**

Run: `make schema-udev parity`
Expected: both compile clean, `-Wall -Wextra`.

- [ ] **Step 5: Commit**

```bash
git add udev_builtins.h udev-parity.h
git commit -m "feat(cdrom_id): wire UB_CDROM dispatch + capability in-scope parity"
```

---

### Task 3: Live gate + full verification

**Files:**
- Create: `tests/verify_cdrom_id_live.sh`
- Modify: `Makefile` (add `cdrom_id.h` to the `parity` dependency line)

**Interfaces:**
- Consumes: `./udev-parity`, `./schema-udev`, real `/run/udev/data`.

- [ ] **Step 1: Add `cdrom_id.h` to the `parity` dependency line**

`Makefile`:

```make
parity: tools/udev-parity.c udev-parity.h udev_db.h udev_rules.h udev_builtins.h ata_id.h v4l_id.h cdrom_id.h schema-udev.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o udev-parity tools/udev-parity.c
```

- [ ] **Step 2: Write `tests/verify_cdrom_id_live.sh`**

```sh
#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev parity

OUT=$(sudo ./udev-parity)
MM=$(echo "$OUT" | sed -n 's/^VALUE MISMATCHES (keys in both, differing value): //p')
MISS=$(echo "$OUT" | sed -n 's/^IN-SCOPE MISSING (device-class aware): //p')
echo "mismatches=$MM inscope-missing=$MISS"
[ "$MM" = "0" ] || { echo "FAIL: $MM value mismatches"; echo "$OUT" | grep '^VALMIS'; exit 1; }
[ "$MISS" = "0" ] || { echo "FAIL: $MISS in-scope missing"; echo "$OUT" | grep '^INSCOPE-MISS'; exit 1; }

sudo rm -rf /run/schema-udev
sudo ./schema-udev & UDPID=$!
sleep 2; sudo kill "$UDPID" 2>/dev/null || true; wait "$UDPID" 2>/dev/null || true

# sr0 optical: capability keys reproduced (anti-hollow)
c=/run/schema-udev/data/b11:0
[ -e "$c" ] || { echo "FAIL: no shadow record for sr0 b11:0"; exit 1; }
grep -q '^E:ID_CDROM=1$' "$c" || { echo "FAIL: sr0 missing ID_CDROM"; cat "$c"; exit 1; }
grep -q '^E:ID_CDROM_DVD_RAM=1$' "$c" || { echo "FAIL: sr0 missing ID_CDROM_DVD_RAM"; exit 1; }
grep -q '^E:ID_CDROM_CD_RW=1$' "$c" || { echo "FAIL: sr0 missing ID_CDROM_CD_RW"; exit 1; }
grep -q '^E:ID_CDROM_RW_REMOVABLE=1$' "$c" || { echo "FAIL: sr0 missing ID_CDROM_RW_REMOVABLE"; exit 1; }

# regression: slices 3a/3b/3c intact
grep -q '^E:ID_ATA=1$' /run/schema-udev/data/b8:0 || { echo "FAIL: ATA disk lost ID_ATA"; exit 1; }
grep -q '^E:ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0$' /run/schema-udev/data/b8:48 || { echo "FAIL: usb disk lost composed serial"; exit 1; }
grep -q '^E:ID_V4L_CAPABILITIES=:capture:$' /run/schema-udev/data/c81:0 || { echo "FAIL: video0 lost v4l caps"; exit 1; }

echo ">> RESULT: PASS (cdrom_id live gate: 0/0, sr0 capabilities reproduced, 3a/3b/3c intact)"
```

Make executable: `chmod +x tests/verify_cdrom_id_live.sh`.

- [ ] **Step 3: Run the live gate**

Run: `./tests/verify_cdrom_id_live.sh`
Expected: `>> RESULT: PASS (cdrom_id live gate: 0/0, sr0 capabilities reproduced, 3a/3b/3c intact)`. On failure the printed `VALMIS`/`INSCOPE-MISS` or missing-key message localizes it.

Note: if a disc is inserted, udev's `/run/udev/data/b11:0` may carry `ID_CDROM_MEDIA_*` keys; those are deferred (`parity_cdrom_media`) so they will NOT count as in-scope-missing. The gate still passes with a disc in.

- [ ] **Step 4: Full unit suite**

Run: `make test`
Expected: all green, including `test_cdrom_id: OK`.

- [ ] **Step 5: vmtest boot rail**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS`. Not PID 1; a regression here means something leaked into the boot rail.

- [ ] **Step 6: Commit**

```bash
git add tests/verify_cdrom_id_live.sh Makefile
git commit -m "test(cdrom_id): live gate (0/0 + sr0 capabilities + 3a/3b/3c intact)"
```

---

## Self-Review

**Spec coverage:**
- `cdrom_id.h` GET CONFIGURATION + pure decode → Task 1. ✓
- Profile→key table + `ID_CDROM=1` base + `RW_REMOVABLE` (profile 0x02) → Task 1 Step 3. ✓
- `UB_CDROM` gate on `sr*`/`scd*` + dispatch → Task 2 Steps 1-2. ✓
- Parity: `ID_CDROM*` hint, capability in-scope, `ID_CDROM_MEDIA_*` deferred → Task 2 Step 3. ✓
- Fixture unit test asserts exactly 17 keys + no media keys → Task 1 Step 1. ✓
- Live gate 0/0 + sr0 capabilities + 3a/3b/3c regression → Task 3. ✓
- vmtest unchanged, boundary untouched → Task 3 Step 5 + Global Constraints. ✓
- Media deferred to 3e → Global Constraints + Task 2 Step 3. ✓

**Type consistency:** `cdrom_id_decode(const uint8_t*, int, struct uevent*)`, `cdrom_get_config(const char*, uint8_t*, size_t, int*)`, `cdrom_id_build(sysroot, devpath, devnode, out)` consistent across `cdrom_id.h`, the dispatch call, and the test. `CEMIT` local macro dedups via `uevent_get` (profiles 0x13/0x14 both emit `ID_CDROM_DVD_RW` once). `UB_CDROM=256` distinct.

**Placeholder scan:** none — all code written in full. The descriptor walk is bounds-checked (`off + 4 + addlen > len` break); the profile loop is bounded by `addlen`.
