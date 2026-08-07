# schema-udev ata_id builtin (sub-project B slice 3a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reimplement udev's `ata_id` (mechanism-only, identity subset) as `ata_id.h`, wired into `run_builtins`, so SATA block disks get `ID_SERIAL`/`ID_MODEL`/`ID_REVISION`/`ID_WWN`/`ID_BUS`/`ID_TYPE`/`ID_ATA` natively, parity-verified against udevd on the ATA chain.

**Architecture:** New header-only `ata_id.h` following the `blkid_fs.h`/`usb_id.h` pattern. A pure `ata_id_decode(const uint8_t buf[512], struct uevent*)` (unit-testable against a captured IDENTIFY fixture) plus `ata_id_identify()` (SG_IO ATA-16 pass-through, device-only) and a thin `ata_id_build()` wrapper. Reuses `usb_plain`/`usb_encode` from `usb_id.h` for byte-parity on model/serial normalization. Dispatched from `udev_builtins.h` via a new `UB_ATA` gate. The parity classifier makes block identity in-scope on the ATA chain and documents the feature-set keys as deferred.

**Tech Stack:** C (C99, `-Wall -Wextra`), Linux SG_IO (`<scsi/sg.h>`), existing schema-init Makefile + test harness, `~/schema-livetest/vmtest.sh`.

## Global Constraints

- **Identity subset only.** Emit `ID_ATA=1`, `ID_BUS=ata`, `ID_TYPE=disk`, `ID_MODEL`, `ID_MODEL_ENC`, `ID_SERIAL`, `ID_SERIAL_SHORT`, `ID_REVISION`, `ID_WWN`, `ID_WWN_WITH_EXTENSION`. Do NOT emit the ~24 `ID_ATA_*` feature-set keys — they are documented-deferred in `parity_ata_feature()`.
- **Reuse, don't re-derive.** Model/serial normalization uses `usb_plain` (trim + ws→`_` + unsafe→`_`) and `usb_encode` (`\xNN`, pad kept) from `usb_id.h`. udev shares these across ata_id/usb_id — reuse guarantees byte-parity.
- **ATA text fields are high-byte-first per 16-bit word.** In the SG_IO buffer (little-endian host), word N low byte is `buf[2N]`, high byte `buf[2N+1]`; the first character of a text field is the HIGH byte. So string decode reads `buf[2N+1]` then `buf[2N]`.
- **Gate distinguishes ATA from USB/SCSI/NVMe:** `SUBSYSTEM==block` + `DEVTYPE==disk` + an `ata[digit]` ancestor path component. sda/sdb/sdc match; the usb disk sdd must NOT.
- **Boundary:** `schema-udev.c`, `schema-udev.h`, and the group-1 netlink bind stay byte-identical. Only `udev_builtins.h`, `udev-parity.h`, `Makefile` change (+ new files).
- **Honesty:** the live gate asserts the parity tool's computed counters AND a positive reproduced-count (≥3 disks). Never loosen the classifier without the builtin actually firing.
- Terse code, no comments beyond the surrounding style. Each builtin resets `out->n=0` and appends via a local `UEMIT` macro (see `usb_id.h`).

## Fixture

`tests/fixtures/ata_sda_identify.h` is committed: `static const uint8_t ata_sda_identify[512]` (real sda IDENTIFY, little-endian byte order as SG_IO returns) with the ground-truth values in its header comment. Tasks reference it directly.

---

### Task 1: `ata_id.h` decode core + unit tests

**Files:**
- Create: `ata_id.h`
- Test: `tests/test_ata_id.c`
- Modify: `Makefile` (add `test_ata_id` to the `test` target)

**Interfaces:**
- Produces: `ata_str_raw(const uint8_t *buf, int w0, int wc, char *raw)` (fills `raw` with `2*wc` chars + NUL, high-byte-first). `ata_id_decode(const uint8_t buf[512], struct uevent *out) -> int` (resets `out->n`, emits the identity subset, returns key count).

- [ ] **Step 1: Write the failing unit test**

Create `tests/test_ata_id.c`:

```c
#include "../ata_id.h"
#include "fixtures/ata_sda_identify.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *get(const struct uevent *ev, const char *k) { return uevent_get(ev, k); }

int main(void) {
    struct uevent ev;
    int n = ata_id_decode(ata_sda_identify, &ev);
    assert(n > 0);
    assert(strcmp(get(&ev, "ID_ATA"), "1") == 0);
    assert(strcmp(get(&ev, "ID_BUS"), "ata") == 0);
    assert(strcmp(get(&ev, "ID_TYPE"), "disk") == 0);
    assert(strcmp(get(&ev, "ID_SERIAL_SHORT"), "WD-WCC6Y2RF681K") == 0);
    assert(strcmp(get(&ev, "ID_MODEL"), "WDC_WD10EZEX-08WN4A0") == 0);
    assert(strcmp(get(&ev, "ID_MODEL_ENC"),
        "WDC\\x20WD10EZEX-08WN4A0\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20"
        "\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20") == 0);
    assert(strcmp(get(&ev, "ID_REVISION"), "02.01A02") == 0);
    assert(strcmp(get(&ev, "ID_SERIAL"), "WDC_WD10EZEX-08WN4A0_WD-WCC6Y2RF681K") == 0);
    assert(strcmp(get(&ev, "ID_WWN"), "0x50014ee211e8fd40") == 0);
    assert(strcmp(get(&ev, "ID_WWN_WITH_EXTENSION"), "0x50014ee211e8fd40") == 0);

    /* no-WWN: clear the WWN-supported bit (word 87 bit 8) and zero words 108-111 */
    uint8_t nw[512];
    memcpy(nw, ata_sda_identify, sizeof nw);
    nw[2*87 + 1] &= ~0x01;                 /* clear bit 8 of word 87 (high byte bit 0) */
    for (int i = 108; i <= 111; i++) { nw[2*i] = 0; nw[2*i + 1] = 0; }
    struct uevent ev2;
    ata_id_decode(nw, &ev2);
    assert(get(&ev2, "ID_WWN") == NULL);
    assert(get(&ev2, "ID_WWN_WITH_EXTENSION") == NULL);
    assert(strcmp(get(&ev2, "ID_MODEL"), "WDC_WD10EZEX-08WN4A0") == 0);  /* rest intact */

    printf("test_ata_id: OK\n");
    return 0;
}
```

Note: word 87 = 0x6123; bit 8 is `0x0100`, i.e. bit 0 of the HIGH byte (`buf[2*87+1]` = 0x61) — clearing `0x01` there removes it.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_ata_id.c -o /tmp/schema-test-ataid && /tmp/schema-test-ataid`
Expected: FAIL — compile error (`ata_id.h` / `ata_id_decode` do not exist).

- [ ] **Step 3: Implement `ata_id.h` decode core**

Create `ata_id.h`:

```c
#ifndef ATA_ID_H
#define ATA_ID_H

#include "usb_id.h"     /* usb_plain, usb_encode (+ transitively schema-udev.h) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline void ata_str_raw(const uint8_t *buf, int w0, int wc, char *raw) {
    int j = 0;
    for (int i = 0; i < wc; i++) {
        raw[j++] = (char)buf[2 * (w0 + i) + 1];   /* high byte = first char */
        raw[j++] = (char)buf[2 * (w0 + i)];       /* low byte  = second char */
    }
    raw[j] = '\0';
}

static inline int ata_id_decode(const uint8_t *buf, struct uevent *out) {
    char raw[64];
    char serial[64], model[64], model_enc[256], rev[32];

    ata_str_raw(buf, 10, 10, raw);  usb_plain(raw, serial, sizeof serial);
    ata_str_raw(buf, 27, 20, raw);  usb_plain(raw, model, sizeof model);
                                    usb_encode(raw, model_enc, sizeof model_enc);
    ata_str_raw(buf, 23, 4, raw);   usb_plain(raw, rev, sizeof rev);

    out->n = 0;
    #define UEMIT(k, v) do { \
        if (out->n < UE_MAX_KEYS) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], (v), UE_VAL_MAX); \
            out->n++; \
        } \
    } while (0)

    UEMIT("ID_ATA", "1");
    UEMIT("ID_TYPE", "disk");
    UEMIT("ID_BUS", "ata");
    if (model[0]) { UEMIT("ID_MODEL", model); UEMIT("ID_MODEL_ENC", model_enc); }
    if (rev[0]) UEMIT("ID_REVISION", rev);
    if (serial[0]) {
        UEMIT("ID_SERIAL_SHORT", serial);
        char full[128];
        if (model[0]) snprintf(full, sizeof full, "%s_%s", model, serial);
        else          safe_copy(full, serial, sizeof full);
        UEMIT("ID_SERIAL", full);
    }

    unsigned w87 = (unsigned)buf[2 * 87] | ((unsigned)buf[2 * 87 + 1] << 8);
    if (w87 & 0x0100) {
        uint64_t wwn = 0;
        for (int i = 0; i < 4; i++) {
            unsigned w = (unsigned)buf[2 * (108 + i)] | ((unsigned)buf[2 * (108 + i) + 1] << 8);
            wwn = (wwn << 16) | w;
        }
        if (wwn) {
            char s[32];
            snprintf(s, sizeof s, "0x%016llx", (unsigned long long)wwn);
            UEMIT("ID_WWN", s);
            UEMIT("ID_WWN_WITH_EXTENSION", s);
        }
    }
    #undef UEMIT
    return out->n;
}

#endif /* ATA_ID_H */
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_ata_id.c -o /tmp/schema-test-ataid && /tmp/schema-test-ataid`
Expected: PASS — `test_ata_id: OK`.

- [ ] **Step 5: Add the test to the Makefile `test` target**

Add a line mirroring the existing per-test lines (e.g. after `test_udev_db`):

```make
	$(CC) $(CFLAGS) tests/test_ata_id.c -o /tmp/schema-test-ataid && /tmp/schema-test-ataid
```

- [ ] **Step 6: Commit**

```bash
git add ata_id.h tests/test_ata_id.c tests/fixtures/ata_sda_identify.h Makefile
git commit -m "feat(ata_id): IDENTIFY decode core + fixture unit tests"
```

---

### Task 2: SG_IO IDENTIFY retrieval + `ata_id_build`

Adds the device-reading half. No unit test (needs a real device / ioctl) — verified by the live gate in Task 4. This task's gate is a clean build.

**Files:**
- Modify: `ata_id.h`

**Interfaces:**
- Produces: `ata_id_identify(const char *devnode, uint8_t buf[512]) -> int` (0 on success, -1 otherwise). `ata_id_build(const char *sysroot, const char *devpath, const char *devnode, struct uevent *out) -> int` — `out->n=0` then, if `devnode` and IDENTIFY succeed, `ata_id_decode`; returns key count (0 if unavailable).

- [ ] **Step 1: Add SG_IO retrieval + build wrapper to `ata_id.h`**

Add these includes to the top of `ata_id.h` (after the existing ones):

```c
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
```

Add before the `#endif` (16-byte ATA_16 cdb: `[0]=0x85` PASS-THROUGH-16, `[1]=0x08` protocol 4 PIO-in, `[2]=0x0e` T_DIR-in|BYTE_BLOCK|T_LENGTH=2, `[6]=0x01` one sector, `[14]=0xec` IDENTIFY):

```c
static inline int ata_id_identify(const char *devnode, uint8_t *buf) {
    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    uint8_t cdb[16] = {0};
    cdb[0] = 0x85; cdb[1] = 0x08; cdb[2] = 0x0e; cdb[6] = 0x01; cdb[14] = 0xec;
    uint8_t sense[32] = {0};
    struct sg_io_hdr io = {0};
    io.interface_id = 'S';
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.cmd_len = sizeof cdb;
    io.cmdp = cdb;
    io.dxfer_len = 512;
    io.dxferp = buf;
    io.sbp = sense;
    io.mx_sb_len = sizeof sense;
    io.timeout = 2000;
    int rc = ioctl(fd, SG_IO, &io);
    close(fd);
    if (rc < 0) return -1;
    if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK) return -1;
    return 0;
}

static inline int ata_id_build(const char *sysroot, const char *devpath,
                               const char *devnode, struct uevent *out) {
    (void)sysroot; (void)devpath;
    out->n = 0;
    if (!devnode) return 0;
    uint8_t buf[512];
    if (ata_id_identify(devnode, buf) != 0) return 0;
    return ata_id_decode(buf, out);
}
```

- [ ] **Step 2: Verify it compiles cleanly**

Run: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. -fsyntax-only -xc ata_id.h`
Expected: no warnings/errors. Re-run the Task 1 unit test to confirm no regression: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_ata_id.c -o /tmp/schema-test-ataid && /tmp/schema-test-ataid` → `test_ata_id: OK`.

- [ ] **Step 3: Commit**

```bash
git add ata_id.h
git commit -m "feat(ata_id): SG_IO ATA-16 IDENTIFY retrieval + build wrapper"
```

---

### Task 3: Wire dispatch + parity classifier

**Files:**
- Modify: `udev_builtins.h`, `udev-parity.h`

**Interfaces:**
- Consumes: `ata_id_build`, and `parity_in_scope_missing` semantics from slice 1.
- Produces: `UB_ATA` bit; `parity_ata_feature(const char *key) -> int`.

- [ ] **Step 1: Add the `UB_ATA` gate to `udev_builtins.h`**

Add `#include "ata_id.h"` with the other builtin includes. Extend the enum:

```c
enum { UB_HWDB = 1, UB_PATH = 2, UB_USB = 4, UB_INPUT = 8, UB_NET = 16, UB_BLKID = 32, UB_ATA = 64 };
```

Add a helper (near `ub_kernel_name`) to detect an `ata[digit]` ancestor segment in the devpath:

```c
static inline int ub_has_ata_ancestor(const char *devpath) {
    for (const char *p = devpath; (p = strstr(p, "/ata")) != NULL; p += 4)
        if (p[4] >= '0' && p[4] <= '9') return 1;
    return 0;
}
```

In `ub_select`, after the `UB_BLKID` block, add:

```c
    if (subsystem && strcmp(subsystem, "block") == 0 &&
        devtype && strcmp(devtype, "disk") == 0 &&
        ub_has_ata_ancestor(devpath))
        sel |= UB_ATA;
```

- [ ] **Step 2: Dispatch `ata_id` in `run_builtins` (before blkid)**

Immediately before the `if (sel & UB_BLKID)` block:

```c
    if (sel & UB_ATA) { tmp.n = 0; ata_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); }
```

- [ ] **Step 3: Add the feature-set deferral + ATA-chain in-scope rule to `udev-parity.h`**

Add `parity_ata_feature`:

```c
static inline int parity_ata_feature(const char *key) {
    if (strncmp(key, "ID_ATA_", 7) != 0) return 0;   /* ID_ATA (=1) is emitted, not deferred */
    return 1;   /* all ID_ATA_* feature-set keys are documented-deferred in slice 3a */
}
```

In `parity_in_scope_missing`, in the `block` branch: defer any `parity_ata_feature(key)`, and make identity keys in-scope when the chain is ATA. Replace the block branch body so it reads:

```c
    if (sub && strcmp(sub, "block") == 0) {
        if (parity_ata_feature(key)) return 0;                 /* slice-3a deferral */
        if (strcmp(key, "ID_PATH") == 0 || strcmp(key, "ID_PATH_TAG") == 0 ||
            strstr(key, "_FROM_DATABASE") != NULL ||
            strncmp(key, "ID_FS_", 6) == 0 || strncmp(key, "ID_PART_", 8) == 0 ||
            strcmp(key, "ID_USB_INTERFACE_NUM") == 0 || strcmp(key, "ID_USB_DRIVER") == 0)
            return 1;
        /* identity keys are ours on the ATA chain (ata_id, slice 3a); on usb/scsi
         * block they remain deferred (scsi_id, slice 3b). */
        if (parity_identity_key(key) && devpath && strstr(devpath, "/ata") != NULL)
            return 1;
        return 0;
    }
```

(The `/ata` substring test mirrors `ub_has_ata_ancestor`; both fire on the same disks. `parity_identity_key` already covers `ID_SERIAL*`/`ID_MODEL*`/`ID_VENDOR*`/`ID_REVISION`/`ID_BUS`/`ID_TYPE`/`ID_WWN`.)

- [ ] **Step 4: Build the daemon and the parity tool**

Run: `make schema-udev parity`
Expected: both compile clean, `-Wall -Wextra`.

- [ ] **Step 5: Commit**

```bash
git add udev_builtins.h udev-parity.h
git commit -m "feat(ata_id): wire UB_ATA dispatch + ATA-chain in-scope parity"
```

---

### Task 4: Live gate + full verification

**Files:**
- Create: `tests/verify_ata_id_live.sh`
- Modify: `Makefile` (parity dep on `ata_id.h`)

**Interfaces:**
- Consumes: `./udev-parity`, real `/run/udev/data`.

- [ ] **Step 1: Add `ata_id.h` to the `parity` target dependency line**

`Makefile` — the `parity:` prerequisite list should include `ata_id.h` (so it rebuilds when the header changes):

```make
parity: tools/udev-parity.c udev-parity.h udev_db.h udev_rules.h udev_builtins.h ata_id.h schema-udev.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o udev-parity tools/udev-parity.c
```

- [ ] **Step 2: Write `tests/verify_ata_id_live.sh`**

Asserts 0/0 counters, a positive reproduced-count on the 3 SATA disks (anti-hollow), and that the usb disk did not gain `ID_ATA`.

```sh
#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s parity

OUT=$(sudo ./udev-parity)

# 1) counters must be 0 (identity now in-scope on ATA)
MM=$(echo "$OUT" | sed -n 's/^VALUE MISMATCHES (keys in both, differing value): //p')
MISS=$(echo "$OUT" | sed -n 's/^IN-SCOPE MISSING (device-class aware): //p')
echo "mismatches=$MM inscope-missing=$MISS"
[ "$MM" = "0" ] || { echo "FAIL: $MM value mismatches"; echo "$OUT" | grep '^VALMIS'; exit 1; }
[ "$MISS" = "0" ] || { echo "FAIL: $MISS in-scope missing"; echo "$OUT" | grep '^INSCOPE-MISS'; exit 1; }

# 2) anti-hollow: our ata_id must actually fire on the SATA disks. Confirm each
#    real ATA disk's ID_WWN is reproduced by re-deriving with the parity tool's
#    own build. Use a tiny probe: coldplug our daemon to the shadow db and check
#    the ATA disks carry ID_MODEL/ID_SERIAL we produced.
sudo rm -rf /run/schema-udev
sudo ./schema-udev & UDPID=$!
sleep 2; sudo kill "$UDPID" 2>/dev/null || true; wait "$UDPID" 2>/dev/null || true

ATA_OK=0
for key in b8:0 b8:16 b8:32; do
    f=/run/schema-udev/data/$key
    [ -e "$f" ] || { echo "FAIL: no shadow record for ATA disk $key"; exit 1; }
    grep -q '^E:ID_ATA=1$' "$f" || { echo "FAIL: $key missing ID_ATA"; exit 1; }
    grep -q '^E:ID_BUS=ata$' "$f" || { echo "FAIL: $key missing ID_BUS=ata"; exit 1; }
    grep -q '^E:ID_MODEL=' "$f" || { echo "FAIL: $key missing ID_MODEL"; exit 1; }
    ATA_OK=$((ATA_OK+1))
done
[ "$ATA_OK" -ge 3 ] || { echo "FAIL: only $ATA_OK ATA disks reproduced"; exit 1; }

# 3) negative: the usb disk (sdd, b8:48) must NOT gain ID_ATA from our builtin
if [ -e /run/schema-udev/data/b8:48 ]; then
    grep -q '^E:ID_ATA=1$' /run/schema-udev/data/b8:48 && { echo "FAIL: ata_id fired on usb disk b8:48"; exit 1; }
fi

echo ">> RESULT: PASS (ata_id live gate: 0/0, $ATA_OK ATA disks, usb untouched)"
```

Make executable: `chmod +x tests/verify_ata_id_live.sh`.

- [ ] **Step 3: Run the live gate**

Run: `./tests/verify_ata_id_live.sh`
Expected: `>> RESULT: PASS (ata_id live gate: 0/0, 3 ATA disks, usb untouched)`. On failure the printed `VALMIS`/`INSCOPE-MISS` lines or the missing-key message localize it.

- [ ] **Step 4: Run the full unit suite**

Run: `make test`
Expected: all suites green, including `test_ata_id: OK`.

- [ ] **Step 5: Run the vmtest boot rail**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS`. schema-udev is not PID 1; a regression here means something leaked into the boot rail.

- [ ] **Step 6: Commit**

```bash
git add tests/verify_ata_id_live.sh Makefile
git commit -m "test(ata_id): live gate (0/0 + 3 ATA disks reproduced + usb untouched)"
```

---

## Self-Review

**Spec coverage:**
- `ata_id.h` decode (identity subset) + IDENTIFY retrieval → Tasks 1, 2. ✓
- Reuse `usb_plain`/`usb_encode` → Task 1 Step 3. ✓
- `UB_ATA` gate distinguishing ATA from usb/nvme → Task 3 Steps 1-2 (`ub_has_ata_ancestor`). ✓
- Feature-set keys documented-deferred → Task 3 Step 3 (`parity_ata_feature`). ✓
- Identity in-scope on ATA chain only → Task 3 Step 3. ✓
- Unit tests vs captured fixture + no-WWN → Task 1. ✓
- Live gate 0/0 + anti-hollow ≥3 reproduced + usb untouched → Task 4. ✓
- vmtest unchanged, boundary (schema-udev.c/.h untouched) → Task 4 Step 5 + Global Constraints. ✓

**Type consistency:** `ata_id_decode(const uint8_t[512], struct uevent*)`, `ata_id_identify(const char*, uint8_t*)`, `ata_id_build(sysroot, devpath, devnode, out)` are consistent across `ata_id.h`, the dispatch call in `udev_builtins.h`, and the test. `UEMIT` local macro matches the `usb_id.h` pattern. `ub_has_ata_ancestor` (dispatch) and the `/ata` substring test (parity) fire on the same disk set.

**Placeholder scan:** none. The SG_IO retrieval is written as a single clean 16-byte-cdb implementation; all logic across tasks is written in full.
