# schema-udev v4l_id builtin (sub-project B slice 3c) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reimplement udev's `v4l_id` as `v4l_id.h`, wired into `run_builtins`, so V4L2 devices get `ID_V4L_VERSION`/`ID_V4L_PRODUCT`/`ID_V4L_CAPABILITIES` natively; remove the parity classifier's `v4l_id` deferral so those keys are in-scope and parity-verified.

**Architecture:** New header-only `v4l_id.h` (ata_id pattern: a pure `v4l_id_decode` split from the `VIDIOC_QUERYCAP` ioctl for unit-testing), dispatched via a new `UB_V4L` gate on `SUBSYSTEM==video4linux`. The classifier drops the v4l blanket-defer.

**Tech Stack:** C (C99, `-Wall -Wextra`), Linux V4L2 (`<linux/videodev2.h>`), existing schema-init Makefile + test harness, `~/schema-livetest/vmtest.sh`.

## Global Constraints

- **v4l_id's complete output is 3 keys:** `ID_V4L_VERSION="2"` (constant on a successful V4L2 QUERYCAP), `ID_V4L_PRODUCT` (raw `card` field, verbatim), `ID_V4L_CAPABILITIES` (`:`-delimited).
- **Effective caps:** use `device_caps` when `capabilities & V4L2_CAP_DEVICE_CAPS`, else `capabilities`. Append `capture`/`output`/`overlay` for the CAPTURE/OUTPUT/OVERLAY bits; `:` when none.
- **Ground truth (blakbox):** video0 `c81:0` → `ID_V4L_VERSION=2`, `ID_V4L_PRODUCT=USB2.0 UVC PC Camera: USB2.0 UV`, `ID_V4L_CAPABILITIES=:capture:`; video1 `c81:1` → same VERSION/PRODUCT, `ID_V4L_CAPABILITIES=:`.
- **Boundary:** `schema-udev.c`, `schema-udev.h`, and the group-1 netlink bind stay byte-identical. Changes: `v4l_id.h` (new), `udev_builtins.h`, `udev-parity.h`, `tests/`, `Makefile`.
- **Honesty:** the live gate asserts the parity tool's computed counters AND positive reproduction of both cap branches (`:capture:` and `:`).
- Terse code, style matches surrounding files.

---

### Task 1: `v4l_id.h` (query + pure decode) + unit tests

**Files:**
- Create: `v4l_id.h`
- Test: `tests/test_v4l_id.c`
- Modify: `Makefile` (add `test_v4l_id` to the `test` target)

**Interfaces:**
- Produces: `v4l_id_query(const char *devnode, struct v4l2_capability *cap) -> int` (0/-1); `v4l_id_decode(const struct v4l2_capability *cap, struct uevent *out) -> int` (resets `out->n`, emits 3 keys, returns count); `v4l_id_build(sysroot, devpath, devnode, out) -> int`.

- [ ] **Step 1: Write the failing unit test**

Create `tests/test_v4l_id.c`:

```c
#include "../v4l_id.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *get(const struct uevent *ev, const char *k) { return uevent_get(ev, k); }

static void cap_init(struct v4l2_capability *c, const char *card,
                     unsigned caps, unsigned dcaps) {
    memset(c, 0, sizeof *c);
    snprintf((char *)c->card, sizeof c->card, "%s", card);
    c->capabilities = caps;
    c->device_caps = dcaps;
}

int main(void) {
    struct v4l2_capability c;
    struct uevent ev;

    /* capture node (uses device_caps because DEVICE_CAPS set) */
    cap_init(&c, "USB2.0 UVC PC Camera: USB2.0 UV",
             V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_DEVICE_CAPS, V4L2_CAP_VIDEO_CAPTURE);
    assert(v4l_id_decode(&c, &ev) == 3);
    assert(strcmp(get(&ev, "ID_V4L_VERSION"), "2") == 0);
    assert(strcmp(get(&ev, "ID_V4L_PRODUCT"), "USB2.0 UVC PC Camera: USB2.0 UV") == 0);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":capture:") == 0);

    /* metadata node: no capture/output/overlay in device_caps -> ":" */
    cap_init(&c, "meta", V4L2_CAP_META_CAPTURE | V4L2_CAP_DEVICE_CAPS, V4L2_CAP_META_CAPTURE);
    v4l_id_decode(&c, &ev);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":") == 0);

    /* DEVICE_CAPS selection: capabilities has CAPTURE but device_caps does not */
    cap_init(&c, "sel", V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_DEVICE_CAPS, 0);
    v4l_id_decode(&c, &ev);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":") == 0);   /* reflects device_caps */

    /* no DEVICE_CAPS bit -> falls back to capabilities */
    cap_init(&c, "old", V4L2_CAP_VIDEO_CAPTURE, 0);
    v4l_id_decode(&c, &ev);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":capture:") == 0);

    /* output + overlay */
    cap_init(&c, "out", V4L2_CAP_DEVICE_CAPS, V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_VIDEO_OVERLAY);
    v4l_id_decode(&c, &ev);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":output:overlay:") == 0);

    printf("test_v4l_id: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_v4l_id.c -o /tmp/schema-test-v4lid && /tmp/schema-test-v4lid`
Expected: FAIL — compile error (`v4l_id.h` / `v4l_id_decode` do not exist).

- [ ] **Step 3: Implement `v4l_id.h`**

Create `v4l_id.h`:

```c
#ifndef V4L_ID_H
#define V4L_ID_H

#include "schema-udev.h"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static inline int v4l_id_decode(const struct v4l2_capability *cap, struct uevent *out) {
    out->n = 0;
    #define VEMIT(k, v) do { \
        if (out->n < UE_MAX_KEYS) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], (v), UE_VAL_MAX); \
            out->n++; \
        } \
    } while (0)

    VEMIT("ID_V4L_VERSION", "2");

    char product[UE_VAL_MAX];
    snprintf(product, sizeof product, "%.32s", (const char *)cap->card);  /* card is 32 bytes */
    VEMIT("ID_V4L_PRODUCT", product);

    unsigned caps = (cap->capabilities & V4L2_CAP_DEVICE_CAPS) ? cap->device_caps
                                                              : cap->capabilities;
    char c[64] = ":";
    if (caps & V4L2_CAP_VIDEO_CAPTURE) safe_copy(c + strlen(c), "capture:", sizeof c - strlen(c));
    if (caps & V4L2_CAP_VIDEO_OUTPUT)  safe_copy(c + strlen(c), "output:",  sizeof c - strlen(c));
    if (caps & V4L2_CAP_VIDEO_OVERLAY) safe_copy(c + strlen(c), "overlay:", sizeof c - strlen(c));
    VEMIT("ID_V4L_CAPABILITIES", c);

    #undef VEMIT
    return out->n;
}

static inline int v4l_id_query(const char *devnode, struct v4l2_capability *cap) {
    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = ioctl(fd, VIDIOC_QUERYCAP, cap);
    close(fd);
    return rc < 0 ? -1 : 0;
}

static inline int v4l_id_build(const char *sysroot, const char *devpath,
                               const char *devnode, struct uevent *out) {
    (void)sysroot; (void)devpath;
    out->n = 0;
    if (!devnode) return 0;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (v4l_id_query(devnode, &cap) != 0) return 0;
    return v4l_id_decode(&cap, out);
}

#endif /* V4L_ID_H */
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_v4l_id.c -o /tmp/schema-test-v4lid && /tmp/schema-test-v4lid`
Expected: PASS — `test_v4l_id: OK`.

- [ ] **Step 5: Add the test to the Makefile `test` target**

Add after the `test_ata_id` line:

```make
	$(CC) $(CFLAGS) tests/test_v4l_id.c -o /tmp/schema-test-v4lid && /tmp/schema-test-v4lid
```

- [ ] **Step 6: Commit**

```bash
git add v4l_id.h tests/test_v4l_id.c Makefile
git commit -m "feat(v4l_id): VIDIOC_QUERYCAP decode (version/product/capabilities)"
```

---

### Task 2: Wire `UB_V4L` dispatch + open parity scope

**Files:**
- Modify: `udev_builtins.h`, `udev-parity.h`

**Interfaces:**
- Consumes: `v4l_id_build`; produces `UB_V4L` bit.

- [ ] **Step 1: Add the `UB_V4L` gate to `udev_builtins.h`**

Add `#include "v4l_id.h"` with the other builtin includes. Extend the enum:

```c
enum { UB_HWDB = 1, UB_PATH = 2, UB_USB = 4, UB_INPUT = 8, UB_NET = 16, UB_BLKID = 32, UB_ATA = 64, UB_V4L = 128 };
```

In `ub_select`, after the `UB_NET` line, add:

```c
    if (subsystem && strcmp(subsystem, "video4linux") == 0) sel |= UB_V4L;
```

- [ ] **Step 2: Dispatch `v4l_id` in `run_builtins`**

After the `if (sel & UB_NET)` line, add:

```c
    if (sel & UB_V4L)   { tmp.n = 0; v4l_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); }
```

- [ ] **Step 3: Remove the v4l blanket-defer in `udev-parity.h`**

In `parity_in_scope_missing`, delete the line:

```c
    if (strcmp(hint, "v4l_id") == 0) return 0;    /* v4l_id not reimplemented */
```

The three `ID_V4L_*` keys (hint `v4l_id`) become in-scope. (`parity_builtin_hint` keeps mapping them to `v4l_id`; nothing else changes.)

- [ ] **Step 4: Build daemon + parity tool**

Run: `make schema-udev parity`
Expected: both compile clean, `-Wall -Wextra`.

- [ ] **Step 5: Commit**

```bash
git add udev_builtins.h udev-parity.h
git commit -m "feat(v4l_id): wire UB_V4L dispatch + v4l keys in-scope parity"
```

---

### Task 3: Live gate + full verification

**Files:**
- Create: `tests/verify_v4l_id_live.sh`
- Modify: `Makefile` (add `v4l_id.h` to the `parity` dependency line)

**Interfaces:**
- Consumes: `./udev-parity`, `./schema-udev`, real `/run/udev/data`.

- [ ] **Step 1: Add `v4l_id.h` to the `parity` dependency line**

`Makefile` — extend the `parity:` prerequisites:

```make
parity: tools/udev-parity.c udev-parity.h udev_db.h udev_rules.h udev_builtins.h ata_id.h v4l_id.h schema-udev.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o udev-parity tools/udev-parity.c
```

- [ ] **Step 2: Write `tests/verify_v4l_id_live.sh`**

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

# video0: capture branch
v=/run/schema-udev/data/c81:0
grep -q '^E:ID_V4L_VERSION=2$' "$v" || { echo "FAIL: video0 missing ID_V4L_VERSION"; exit 1; }
grep -q '^E:ID_V4L_PRODUCT=USB2.0 UVC PC Camera: USB2.0 UV$' "$v" || { echo "FAIL: video0 wrong ID_V4L_PRODUCT"; cat "$v"; exit 1; }
grep -q '^E:ID_V4L_CAPABILITIES=:capture:$' "$v" || { echo "FAIL: video0 wrong ID_V4L_CAPABILITIES"; exit 1; }

# video1: empty-caps branch
v1=/run/schema-udev/data/c81:1
grep -q '^E:ID_V4L_CAPABILITIES=:$' "$v1" || { echo "FAIL: video1 ID_V4L_CAPABILITIES not ':'"; cat "$v1"; exit 1; }

# regression: slices 3a/3b intact
grep -q '^E:ID_ATA=1$' /run/schema-udev/data/b8:0 || { echo "FAIL: ATA disk lost ID_ATA"; exit 1; }
grep -q '^E:ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0$' /run/schema-udev/data/b8:48 || { echo "FAIL: usb disk lost composed serial"; exit 1; }

echo ">> RESULT: PASS (v4l_id live gate: 0/0, video0 :capture: + video1 :, 3a/3b intact)"
```

Make executable: `chmod +x tests/verify_v4l_id_live.sh`.

- [ ] **Step 3: Run the live gate**

Run: `./tests/verify_v4l_id_live.sh`
Expected: `>> RESULT: PASS (v4l_id live gate: 0/0, video0 :capture: + video1 :, 3a/3b intact)`. On failure the printed `VALMIS`/`INSCOPE-MISS` or missing-key message localizes it.

- [ ] **Step 4: Full unit suite**

Run: `make test`
Expected: all green, including `test_v4l_id: OK`.

- [ ] **Step 5: vmtest boot rail**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS`. Not PID 1; a regression here means something leaked into the boot rail.

- [ ] **Step 6: Commit**

```bash
git add tests/verify_v4l_id_live.sh Makefile
git commit -m "test(v4l_id): live gate (0/0 + video0 :capture: + video1 : + 3a/3b intact)"
```

---

## Self-Review

**Spec coverage:**
- `v4l_id.h` query + pure decode → Task 1. ✓
- 3 keys (VERSION const, PRODUCT raw card, CAPABILITIES effective-caps) → Task 1 Step 3. ✓
- `UB_V4L` gate on video4linux + dispatch → Task 2 Steps 1-2. ✓
- Remove v4l blanket-defer → Task 2 Step 3. ✓
- Unit tests: both cap branches + DEVICE_CAPS selection + output/overlay → Task 1 Step 1. ✓
- Live gate 0/0 + video0/video1 branches + 3a/3b regression → Task 3. ✓
- vmtest unchanged, boundary (schema-udev.c/.h untouched) → Task 3 Step 5 + Global Constraints. ✓

**Type consistency:** `v4l_id_decode(const struct v4l2_capability*, struct uevent*)`, `v4l_id_query(const char*, struct v4l2_capability*)`, `v4l_id_build(sysroot, devpath, devnode, out)` consistent across `v4l_id.h`, the dispatch call, and the test. `VEMIT` local macro matches the builtin pattern. `UB_V4L=128` distinct from existing bits.

**Placeholder scan:** none — all code written in full. The `card` copy uses `%.32s` precision so it is bounded even if the field is not NUL-terminated.
