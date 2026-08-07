# schema-udev sub-project B slice 3a — ata_id builtin

**Status:** design approved 2026-08-07. Endgame arc: udevd retirement. Slices 1 (property completeness, #89) and 2 (shadow db writer, #90) landed. Slice 3 reimplements the builtins the parity classifier defers; it is decomposed into **3a ata_id** (this spec), **3b scsi_id**, **3c v4l_id**. `cdrom_id` + `mtd_probe` are deferred until there is hardware on blakbox to parity-verify against.

## Goal

Reimplement udev's `ata_id` (mechanism-only) as `ata_id.h`, wired into `run_builtins`, so SATA block disks get their **identity** properties (`ID_SERIAL`/`ID_MODEL`/`ID_REVISION`/`ID_WWN`/`ID_BUS`/`ID_TYPE`/`ID_ATA`) natively. This closes the device-class exclusion that currently marks block identity keys out-of-scope: after 3a, identity keys become **in-scope on the ATA chain** and parity-verified against udevd. USB/SCSI block identity stays deferred (that is slice 3b/scsi_id).

## Scope

### In scope
- New `ata_id.h`: SG_IO ATA-16 pass-through IDENTIFY retrieval + identity decode.
- `udev_builtins.h`: new `UB_ATA` bit, gate in `ub_select`, one dispatch call in `run_builtins`.
- `udev-parity.h`: identity keys become in-scope on the ATA chain; ~24 `ID_ATA_*` feature-set keys added to a documented deferral predicate.
- Unit tests (`tests/test_ata_id.c`) against captured real IDENTIFY fixtures.
- Live gate (`tests/verify_ata_id_live.sh`) + Makefile test target.

### Emitted keys (identity subset)
`ID_ATA=1`, `ID_BUS=ata`, `ID_TYPE=disk`, `ID_MODEL`, `ID_MODEL_ENC`, `ID_SERIAL`, `ID_SERIAL_SHORT`, `ID_REVISION`, `ID_WWN`, `ID_WWN_WITH_EXTENSION` (last two only when the drive reports WWN).

### Out of scope (documented deferral — NOT emitted this slice)
The ~24 ATA feature-set keys: `ID_ATA_WRITE_CACHE(_ENABLED)`, `ID_ATA_READ_LOOKAHEAD(_ENABLED)`, `ID_ATA_FEATURE_SET_{HPA,PM,SECURITY,SMART,APM}(_ENABLED and sub-keys)`, `ID_ATA_FEATURE_SET_SECURITY_ERASE_UNIT_MIN`, `ID_ATA_FEATURE_SET_SECURITY_ENHANCED_ERASE_UNIT_MIN`, `ID_ATA_FEATURE_SET_APM_CURRENT_VALUE`, `ID_ATA_SATA(_SIGNAL_RATE_GEN1/GEN2)`, `ID_ATA_ROTATION_RATE_RPM`, `ID_ATA_DOWNLOAD_MICROCODE`, `ID_ATA_PERIPHERAL_DEVICE_TYPE`. Added to `parity_ata_feature()` deferral (honest, same pattern as blkid geometry). A future micro-slice can fill these if ever wanted.

### Also out of scope
- `scsi_id` (usb/scsi block identity — slice 3b), `v4l_id` (3c), `cdrom_id`/`mtd_probe` (deferred).
- ATAPI/`sr*` handling (IDENTIFY PACKET) — `sr*` is already excluded from the blkid gate; ata_id does not fire on it here.
- Group-2 rebroadcast (near-cutover), any socket change. Group-1 bind stays byte-identical.

## Mechanism: `ata_id.h`

Header-only, `blkid_fs.h` pattern. Includes `usb_id.h` to reuse the exact udev string normalizers (`usb_plain` = replace-whitespace-then-chars → `ID_MODEL`; `usb_encode` = `\xNN` form → `ID_MODEL_ENC`). udev itself shares these across ata_id/usb_id, so reuse guarantees byte-parity. Additional includes: `<scsi/sg.h>`, `<sys/ioctl.h>`, `<fcntl.h>`, `<unistd.h>`, `<stdint.h>`, `<string.h>`, `<stdio.h>`.

### IDENTIFY retrieval
`ata_id_identify(const char *devnode, uint8_t buf[512]) -> int`:
- `open(devnode, O_RDONLY|O_NONBLOCK|O_CLOEXEC)`; on failure return -1.
- Issue **ATA-16 PASS-THROUGH** via `SG_IO` (12-byte cdb): `cdb[0]=0x85` (ATA PASS-THROUGH 16), `cdb[1]=0x08` (protocol 4 = PIO Data-In: `(4<<1)|extend0`), `cdb[2]=0x0E` (T_DIR=from-dev `<<3`, BYTE_BLOCK `<<2`, T_LENGTH=2 in sector_count → `0x08|0x04|0x02`), `cdb[6]=0x01` (1 sector), `cdb[14]=0xEC` (IDENTIFY DEVICE); other cdb bytes 0. `dxfer_direction=SG_DXFER_FROM_DEV`, `dxfer_len=512`, `dxferp=buf`, `timeout=2000ms`. Return 0 on `SG_IO` success with `status==0`, else -1. (Matches udev's `disk_identify` ATA-16 path.)
- No retry with ATA-12 this slice; if ATA-16 fails the device simply yields no ata_id keys (the gate still only fires on ATA disks, so this is rare and non-fatal).

### Decode (identity subset)
IDENTIFY is 256 little-endian words; **ATA text fields are stored high-byte-first within each 16-bit word** (word `0xWXYZ` → chars `0xWX`, `0xYZ`). Helper `ata_str(buf, word_start, word_count, out)` extracts and preserves the raw (with trailing spaces) for `_ENC`, and produces the plain/trimmed form via `usb_plain`.

- Serial: words 10–19 (20 bytes) → raw → `ID_SERIAL_SHORT` = `usb_plain` (trimmed).
- Firmware: words 23–26 (8 bytes) → `usb_plain` → `ID_REVISION`.
- Model: words 27–46 (40 bytes) → `ID_MODEL` = `usb_plain`, `ID_MODEL_ENC` = `usb_encode` of the raw (trailing pad kept).
- `ID_SERIAL` = `<ID_MODEL>_<ID_SERIAL_SHORT>`.
- WWN: words 108–111 assembled MSW-first into a u64; emit `ID_WWN`/`ID_WWN_WITH_EXTENSION` = `0x%016llx` **only when the WWN-supported bit is set** (ATA: word 87 bit 8; the exact word/bit is confirmed against the fixtures and the live gate — the live gate on 3 disks is the authority, per the slice-1 lesson that live parity catches spec bugs). When unset, emit neither WWN key.
- Fixed: `ID_ATA=1`, `ID_BUS=ata`, `ID_TYPE=disk`.

`ata_id_build(const char *sysroot, const char *devpath, const char *devnode, struct uevent *out)`: resets `out->n=0` (builtin owns its scratch, per `ub_absorb` contract), returns early (0 keys) if `devnode` is NULL or IDENTIFY fails, else fills `out` and returns the count.

### Ground-truth fixture (sda, captured 2026-08-07)
The 256-word IDENTIFY buffer is captured (`scratchpad/sda_identify.hex`, via `hdparm --Istdout /dev/sda`) and baked into the unit test as a byte array. Expected decoded values:
- `ID_SERIAL_SHORT=WD-WCC6Y2RF681K`
- `ID_MODEL=WDC_WD10EZEX-08WN4A0`
- `ID_MODEL_ENC=WDC\x20WD10EZEX-08WN4A0\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20`
- `ID_REVISION=02.01A02`
- `ID_SERIAL=WDC_WD10EZEX-08WN4A0_WD-WCC6Y2RF681K`
- `ID_WWN=0x50014ee211e8fd40`, `ID_WWN_WITH_EXTENSION=0x50014ee211e8fd40`

## Dispatch: `udev_builtins.h`

- Add `UB_ATA = 64` to the enum.
- `ub_select`: set `UB_ATA` when `SUBSYSTEM==block`, `DEVTYPE==disk`, and the device chain has an `ata[N]` ancestor segment. Detection: the devpath contains a path component matching `ata` followed by a digit (e.g. `/devices/pci.../ata1/host0/.../block/sda`). This distinguishes SATA (sda/sdb/sdc match) from the USB disk (sdd, no `/ataN/` ancestor) and from NVMe/virtual. It does NOT depend on DEVNAME.
- `run_builtins`: after the `UB_USB`/before the `UB_BLKID` block (udev runs ata_id before blkid), add:
  ```c
  if (sel & UB_ATA) { tmp.n = 0; ata_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); }
  ```
  `devnode` is already threaded through `run_builtins` (blkid uses it).
- Include `#include "ata_id.h"` at the top of `udev_builtins.h`.

## Parity classifier: `udev-parity.h`

- New `parity_ata_feature(const char *key)`: returns 1 for the ~24 deferred `ID_ATA_*` feature-set keys listed above (documented deferral). `ID_ATA` itself (=1) is NOT deferred (we emit it).
- `parity_in_scope_missing`: on `block`, identity keys become **in-scope when the chain is ATA** (devpath has an `ata[N]` ancestor segment) — mirror the `ub_select` detection. On block+usb (no ata ancestor), identity keys stay out-of-scope (slice 3b). Feature-set keys via `parity_ata_feature` are always out-of-scope this slice. Keep the existing topology/db/interface allowances.
- The existing usb-chain identity rule (usb devices) is unchanged.

## Verification

### Unit tests — `tests/test_ata_id.c`
- **sda fixture** (baked byte array from `scratchpad/sda_identify.hex`): `ata_id`-decode the buffer directly (a decode entry point that takes a `uint8_t[512]` so no device is needed), assert every identity key equals the ground-truth values above, byte-for-byte (including `ID_MODEL_ENC` trailing `\x20` pad).
- **no-WWN fixture**: a copy of sda's buffer with the WWN-supported bit cleared and words 108–111 zeroed → assert neither `ID_WWN` nor `ID_WWN_WITH_EXTENSION` is emitted, other keys unchanged.
- **byte-swap/trim**: assert `ID_SERIAL_SHORT` is trimmed (leading spaces from words 10–11 removed) and `ID_MODEL` underscores internal spaces while `ID_MODEL_ENC` keeps them as `\x20`.

To make the decode unit-testable without a device, `ata_id.h` exposes `ata_id_decode(const uint8_t buf[512], struct uevent *out)` (pure), and `ata_id_build` = `ata_id_identify` + `ata_id_decode`.

### Live gate — `tests/verify_ata_id_live.sh` (sudo)
- Run coldplug (or `./udev-parity` directly — it runs builtins on every `/sys` device).
- Assert the parity tool reports `IN-SCOPE MISSING (device-class aware): 0` and `VALUE MISMATCHES: 0` (identity now in-scope on ATA; a decode bug shows as a mismatch, a gate/dispatch bug as in-scope-missing).
- **Anti-hollow positive check:** assert the 3 SATA disks (b8:0/b8:16/b8:32) have their identity keys *reproduced* — grep the tool's per-subsystem `reproduced` count or add an explicit "ata identity reproduced on N disks" line; require N≥3. An inert/no-op build would leave identity in-scope-missing and fail, but the positive count also guards against the classifier being loosened without the builtin actually firing.
- **Negative check:** assert sdd (b8:48, usb) did NOT gain `ID_ATA` from our builtin (ata_id must not fire on the usb disk) — its identity stays deferred.
- Ground truth (from `/run/udev/data`): sda/sdb/sdc `ID_BUS=ata` + WWN present; sdd `ID_BUS=usb`, no `ID_ATA`.

### vmtest
Not PID 1 — boot rail must pass unchanged. `cd ~/schema-livetest && ./vmtest.sh`, PASS = timer fired + hang excised + dependent ran + SDBOOTED-DIR present.

## Error handling
- IDENTIFY open/ioctl failure → `ata_id_build` returns 0 keys; dispatch continues (never crashes or drops the event).
- Non-ATA disk that somehow matches the gate → IDENTIFY fails → 0 keys (safe).
- `usb_plain`/`usb_encode` bounded by their output buffers (existing behavior).

## Corrections applied during review
*(populated post-Greg, as in slices 1–2)*

## Boundary summary
- New: `ata_id.h`, `tests/test_ata_id.c`, `tests/verify_ata_id_live.sh`.
- Modified: `udev_builtins.h` (`UB_ATA` enum + gate + include + one dispatch call), `udev-parity.h` (`parity_ata_feature` + ATA-chain in-scope rule), `Makefile` (test target + parity dep on `ata_id.h`).
- Untouched: `schema-udev.c`, `schema-udev.h`, the group-1 netlink bind, `udev_rules.h`, `udev_db.h`, all other builtin headers.
