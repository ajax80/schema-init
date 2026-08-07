# schema-udev sub-project B slice 3c — v4l_id builtin

**Status:** design approved 2026-08-07. Endgame arc: udevd retirement. Slice 3 (deferred builtins) = 3a ata_id (#91), 3b usb-storage id (#92), **3c v4l_id** (this spec, last piece). `cdrom_id`, `mtd_probe`, true `scsi_id` remain deferred (no hardware on blakbox).

## Goal

Reimplement udev's `v4l_id` (mechanism-only) as `v4l_id.h`, wired into `run_builtins`, so V4L2 devices get their `ID_V4L_VERSION`/`ID_V4L_PRODUCT`/`ID_V4L_CAPABILITIES` natively. Remove the parity classifier's `v4l_id` blanket-deferral so those keys become in-scope and parity-verified. This closes the last device-class exclusion in slice 3.

## Scope

### In scope
- New `v4l_id.h`: `VIDIOC_QUERYCAP` retrieval + a pure decoder.
- `udev_builtins.h`: `UB_V4L` bit, gate (`SUBSYSTEM==video4linux`), one dispatch call.
- `udev-parity.h`: remove the `v4l_id` blanket-defer so `ID_V4L_*` are in-scope.
- Unit tests (`tests/test_v4l_id.c`) against synthetic `v4l2_capability` structs.
- Live gate (`tests/verify_v4l_id_live.sh`) + Makefile test target.

### Emitted keys (v4l_id's complete output)
- `ID_V4L_VERSION` = `"2"` — hardcoded on a successful V4L2 `VIDIOC_QUERYCAP` (matches udev; V4L1 is obsolete and not handled).
- `ID_V4L_PRODUCT` = the `card` field verbatim (raw, NUL-terminated; spaces/colon preserved, e.g. `USB2.0 UVC PC Camera: USB2.0 UV`).
- `ID_V4L_CAPABILITIES` = `:`-delimited capability list (see decode).

### Out of scope
- Non-v4l identity (`ID_BUS`/`ID_MODEL`/`ID_USB_*`/`ID_PATH`) — already produced by usb_id/path_id + rules-engine inheritance (video4linux is in `rules_import_subsystem_inherits`).
- `ID_FOR_SEAT` (logind/uaccess runtime tag — slice D), `ID_PATH_WITH_USB_REVISION` (already in `parity_deferred`).
- Group-2 rebroadcast, any socket change. Group-1 bind stays byte-identical.

## Mechanism: `v4l_id.h`

Header-only, `ata_id.h` pattern (pure decoder split from the ioctl for unit-testing). Includes `schema-udev.h` (uevent, `safe_copy`), `<linux/videodev2.h>` (`struct v4l2_capability`, `VIDIOC_QUERYCAP`, `V4L2_CAP_*`), `<sys/ioctl.h>`, `<fcntl.h>`, `<unistd.h>`, `<string.h>`, `<stdio.h>`.

```c
/* open devnode + VIDIOC_QUERYCAP. Returns 0 and fills *cap; -1 on failure. */
int v4l_id_query(const char *devnode, struct v4l2_capability *cap);

/* Pure decoder (no device). Resets out->n; emits the 3 ID_V4L_* keys; returns count. */
int v4l_id_decode(const struct v4l2_capability *cap, struct uevent *out);

/* Wrapper: out->n=0; 0 keys if !devnode or query fails; else v4l_id_decode. */
int v4l_id_build(const char *sysroot, const char *devpath, const char *devnode,
                 struct uevent *out);
```

### `v4l_id_query`
`open(devnode, O_RDONLY|O_NONBLOCK|O_CLOEXEC)`; `ioctl(fd, VIDIOC_QUERYCAP, cap)`; close. `VIDIOC_QUERYCAP` is a read-only capability query — it does not start streaming and is what udev issues on every add. Return -1 on open/ioctl failure.

### `v4l_id_decode`
- `ID_V4L_VERSION` = `"2"` (constant).
- `ID_V4L_PRODUCT` = `cap->card` (copy verbatim via `safe_copy`; it is a fixed 32-byte NUL-terminated field — raw, not normalized).
- `ID_V4L_CAPABILITIES`: pick the effective caps — `(cap->capabilities & V4L2_CAP_DEVICE_CAPS) ? cap->device_caps : cap->capabilities`. Build a string starting with `:`, then append `capture:` if `V4L2_CAP_VIDEO_CAPTURE`, `output:` if `V4L2_CAP_VIDEO_OUTPUT`, `overlay:` if `V4L2_CAP_VIDEO_OVERLAY`. Result is `:` when none set. (Matches udev: video0 → `:capture:`, video1 metadata node → `:`.)

`v4l_id_build` resets `out->n=0`, returns 0 keys if `devnode` is NULL or `v4l_id_query` fails, else runs `v4l_id_decode`. `sysroot`/`devpath` unused (`(void)` cast).

## Dispatch: `udev_builtins.h`
- Add `UB_V4L = 128` to the enum.
- `ub_select`: set `UB_V4L` when `SUBSYSTEM==video4linux`.
- `run_builtins`: add `if (sel & UB_V4L) { tmp.n = 0; v4l_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); }` (order among builtins is irrelevant — disjoint keys, first-writer-wins). `devnode` is already threaded.
- Add `#include "v4l_id.h"`.

## Parity classifier: `udev-parity.h`
Remove the line `if (strcmp(hint, "v4l_id") == 0) return 0;` from `parity_in_scope_missing` (v4l_id is now reimplemented). The three `ID_V4L_*` keys (hint `v4l_id`) thereby become in-scope. No other v4l keys exist on blakbox targets; the live gate confirms 0 in-scope-missing.

## Verification

### Unit tests — `tests/test_v4l_id.c`
Build synthetic `struct v4l2_capability` values and call `v4l_id_decode` (no device needed):
- **capture node:** `card="USB2.0 UVC PC Camera: USB2.0 UV"`, `capabilities=V4L2_CAP_VIDEO_CAPTURE|V4L2_CAP_DEVICE_CAPS`, `device_caps=V4L2_CAP_VIDEO_CAPTURE` → assert `ID_V4L_VERSION=2`, `ID_V4L_PRODUCT` verbatim, `ID_V4L_CAPABILITIES=:capture:`.
- **metadata node (no video caps):** `device_caps` has neither capture/output/overlay (e.g. `V4L2_CAP_META_CAPTURE|V4L2_CAP_DEVICE_CAPS`) → `ID_V4L_CAPABILITIES=:`.
- **DEVICE_CAPS selection:** `capabilities` includes CAPTURE but `device_caps` does not (with `DEVICE_CAPS` set) → capabilities string reflects `device_caps` (`:`), proving the effective-caps selection.
- **output+overlay:** `device_caps=V4L2_CAP_VIDEO_OUTPUT|V4L2_CAP_VIDEO_OVERLAY|V4L2_CAP_DEVICE_CAPS` → `:output:overlay:`.

### Live gate — `tests/verify_v4l_id_live.sh` (sudo)
- `./udev-parity`: assert `VALUE MISMATCHES: 0` and `IN-SCOPE MISSING: 0` (v4l keys now in-scope; a decode bug shows as a mismatch, a dispatch/gate miss as in-scope-missing).
- Coldplug to the shadow db; **anti-hollow positive:** assert `c81:0` (video0) carries `ID_V4L_VERSION=2`, `ID_V4L_PRODUCT=USB2.0 UVC PC Camera: USB2.0 UV`, `ID_V4L_CAPABILITIES=:capture:`; and `c81:1` (video1) carries `ID_V4L_CAPABILITIES=:` (both branches exercised).
- **Regression guard:** ATA disks (`b8:0`) still `ID_ATA=1`; usb disk (`b8:48`) still `ID_SERIAL=...-0:0` (slices 3a/3b intact).

### vmtest
Not PID 1 — boot rail must pass unchanged. `cd ~/schema-livetest && ./vmtest.sh`.

### Boundary
`schema-udev.c`, `schema-udev.h`, and the group-1 netlink bind stay byte-identical. Changes: `v4l_id.h` (new), `udev_builtins.h` (gate), `udev-parity.h` (remove defer), `tests/` (unit + live), `Makefile` (test target + parity dep).

## Error handling
- `v4l_id_query` open/ioctl failure → `v4l_id_build` returns 0 keys; dispatch continues.
- A non-v4l device mis-gated → query fails → 0 keys (safe). Gate is `SUBSYSTEM==video4linux`, so this cannot happen in practice.
- `card` field: `safe_copy` bounds the copy to `UE_VAL_MAX`.

## Corrections applied during review
*(populated post-Greg, as in prior slices)*
