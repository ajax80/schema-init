# schema-udev sub-project B slice 3d — cdrom_id (drive capabilities)

**Status:** design approved 2026-08-07. Endgame arc: udevd retirement. Slice 3 deferred-builtins: 3a ata_id (#91), 3b usb-storage id (#92), 3c v4l_id (#93), **3d cdrom_id capabilities** (this spec), 3e cdrom_id media (next, needs a disc). An external USB DVD burner (sr0, MATSHITA DVD-RAM UJ8D1) is now a live target, so cdrom_id is no longer deferred-for-lack-of-hardware.

## Goal

Reimplement udev's `cdrom_id` **drive-capability** detection (mechanism-only) as `cdrom_id.h`, wired into `run_builtins`, so optical drives get their `ID_CDROM_*` capability properties natively via MMC GET CONFIGURATION. Bring the capability keys in-scope in the parity classifier; explicitly defer the media-status keys (`ID_CDROM_MEDIA_*`) to slice 3e.

## Scope

### In scope
- New `cdrom_id.h`: SG_IO GET CONFIGURATION retrieval + a pure profile-list decoder.
- `udev_builtins.h`: `UB_CDROM` bit, gate (`SUBSYSTEM==block` + kernel name `sr*`/`scd*`), one dispatch call.
- `udev-parity.h`: `ID_CDROM*`→`cdrom_id` hint; capability `ID_CDROM_*` in-scope on the optical device; `ID_CDROM_MEDIA_*` documented-deferred (3e).
- Unit tests (`tests/test_cdrom_id.c`) against the captured GET CONFIGURATION fixture.
- Live gate (`tests/verify_cdrom_id_live.sh`) + Makefile test target.

### Emitted keys (drive capabilities) — the 17 ground-truth keys for sr0
`ID_CDROM=1` (base), `ID_CDROM_CD`, `ID_CDROM_CD_R`, `ID_CDROM_CD_RW`, `ID_CDROM_DVD`, `ID_CDROM_DVD_R`, `ID_CDROM_DVD_RAM`, `ID_CDROM_DVD_RW`, `ID_CDROM_DVD_RW_RO`, `ID_CDROM_DVD_RW_SEQ`, `ID_CDROM_DVD_R_DL`, `ID_CDROM_DVD_R_DL_SEQ`, `ID_CDROM_DVD_R_DL_JR`, `ID_CDROM_DVD_PLUS_R`, `ID_CDROM_DVD_PLUS_RW`, `ID_CDROM_DVD_PLUS_R_DL`, `ID_CDROM_RW_REMOVABLE`. All values `"1"`.

### Out of scope
- **Media status** (`ID_CDROM_MEDIA`, `ID_CDROM_MEDIA_*`, track/session counts, blank/appendable/complete state) — slice 3e (needs READ DISC INFORMATION + READ TOC and a disc inserted). Deferred honestly in the classifier.
- Non-capability profiles not present on this drive (BD/HD-DVD/MO) — the ported table covers them for completeness but they are untested here; the live gate only asserts what sr0 reports.
- The usb_id identity for sr0 (`ID_BUS=usb`, `ID_SERIAL=...-0:0`, `ID_USB_*`) — already produced by the slice-3b usb_id-on-block gate (sr0 is `DEVTYPE=disk` with a usb ancestor). Verified separately; unchanged here.
- Group-2 rebroadcast, any socket change. Group-1 bind stays byte-identical.

## Mechanism: `cdrom_id.h`

Header-only, `ata_id.h` pattern (pure decoder split from the ioctl). Includes `schema-udev.h`, `<scsi/sg.h>`, `<sys/ioctl.h>`, `<fcntl.h>`, `<unistd.h>`, `<stdint.h>`, `<string.h>`, `<stdio.h>`.

```c
/* SG_IO GET CONFIGURATION (0x46, RT=0). Fills buf (caller-sized), sets *len to
 * the bytes returned (min of allocation and the response data-length+4).
 * Returns 0 on success, -1 on open/ioctl failure. */
int cdrom_get_config(const char *devnode, uint8_t *buf, size_t bufsz, int *len);

/* Pure decoder (no device). Parses the feature-descriptor blob, finds the
 * Profile List (feature 0x0000) and the Removable Medium feature (0x0003),
 * maps each supported profile to ID_CDROM_* capability keys, emits ID_CDROM=1.
 * Resets out->n. Returns key count. */
int cdrom_id_decode(const uint8_t *buf, int len, struct uevent *out);

/* Wrapper: out->n=0; 0 keys if !devnode or GET CONFIGURATION fails. */
int cdrom_id_build(const char *sysroot, const char *devpath, const char *devnode,
                   struct uevent *out);
```

### GET CONFIGURATION (`cdrom_get_config`)
10-byte cdb: `[0]=0x46`, `[1]=0x00` (RT=0, all features), `[2..3]=0x0000` (starting feature), `[7..8]=allocation length` (buffer size, big-endian), rest 0. `open(O_RDONLY|O_NONBLOCK|O_CLOEXEC)`, `SG_IO`, `SG_DXFER_FROM_DEV`, timeout 5000ms. On success set `*len = min(bufsz, ((buf[0]<<24|buf[1]<<16|buf[2]<<8|buf[3]) + 4))`.

### Decode (`cdrom_id_decode`)
The blob is: 8-byte header (bytes 0–3 data length, bytes 6–7 current profile — **ignored this slice**, it is media state → 3e), then feature descriptors. Each feature descriptor header is 4 bytes: feature code (bytes 0–1, big-endian), byte 2 (version/persistent/current), byte 3 additional length; followed by `additional length` bytes of feature data. Walk descriptors by `4 + additional_length`.

- **Profile List** (feature `0x0000`): its data is a series of 4-byte profile descriptors — 2-byte profile number (big-endian) + 1 byte (bit0 = currentp) + reserved. For each profile number, set the mapped `ID_CDROM_*` key(s) via the ported table below. Emit `ID_CDROM=1` once any profile is seen (the device is an MMC/optical unit).
- **Removable Medium** (feature `0x0003`) present → the drive takes removable media; used to pin `ID_CDROM_RW_REMOVABLE` (confirmed against the fixture — the 14 profiles yield 16 keys; `ID_CDROM_RW_REMOVABLE` is the 17th and derives from the removable-medium capability, not a profile).

**Profile → key table (ported from udev cdrom_id):**
| profile | key(s) |
|---|---|
| 0x08 | `ID_CDROM_CD` |
| 0x09 | `ID_CDROM_CD_R` |
| 0x0A | `ID_CDROM_CD_RW` |
| 0x10 | `ID_CDROM_DVD` |
| 0x11 | `ID_CDROM_DVD_R` |
| 0x12 | `ID_CDROM_DVD_RAM` |
| 0x13 | `ID_CDROM_DVD_RW` + `ID_CDROM_DVD_RW_RO` |
| 0x14 | `ID_CDROM_DVD_RW` + `ID_CDROM_DVD_RW_SEQ` |
| 0x15 | `ID_CDROM_DVD_R_DL` + `ID_CDROM_DVD_R_DL_SEQ` |
| 0x16 | `ID_CDROM_DVD_R_DL` + `ID_CDROM_DVD_R_DL_JR` |
| 0x1A | `ID_CDROM_DVD_PLUS_RW` |
| 0x1B | `ID_CDROM_DVD_PLUS_R` |
| 0x2A | `ID_CDROM_DVD_PLUS_RW_DL` |
| 0x2B | `ID_CDROM_DVD_PLUS_R_DL` |
| 0x40 | `ID_CDROM_BD` |
| 0x41,0x42 | `ID_CDROM_BD_R` |
| 0x43 | `ID_CDROM_BD_RE` |
| 0x50 | `ID_CDROM_HDDVD` |
| 0x51 | `ID_CDROM_HDDVD_R` |
| 0x52 | `ID_CDROM_HDDVD_RW` |

(Profiles absent from sr0 — BD/HD-DVD/DVD+RW-DL — are in the table for completeness but untested here.) The **fixture unit test asserts exactly the 17 sr0 keys**, so a wrong table entry or a missing `RW_REMOVABLE` source fails the test.

`cdrom_id_build` resets `out->n=0`, returns 0 keys if `devnode` is NULL or GET CONFIGURATION fails, else `cdrom_id_decode`. `sysroot`/`devpath` unused (`(void)`).

## Dispatch: `udev_builtins.h`
- Add `UB_CDROM = 256` to the enum.
- `ub_select`: set `UB_CDROM` when `SUBSYSTEM==block` and the kernel name matches `sr*` or `scd*` (`fnmatch`). Mirrors udev's `60-cdrom_id.rules` (`KERNEL=="sr[0-9]*|scd[0-9]*"`).
- `run_builtins`: add `if (sel & UB_CDROM) { tmp.n=0; cdrom_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); }` (needs `devnode`).
- Add `#include "cdrom_id.h"`.

## Parity classifier: `udev-parity.h`
- `parity_builtin_hint`: map `ID_CDROM` (prefix) → `cdrom_id`.
- New `parity_cdrom_media(key)`: true for `ID_CDROM_MEDIA` and any `ID_CDROM_MEDIA_*` (deferred to 3e).
- `parity_in_scope_missing`: in the `block` branch, defer `parity_cdrom_media(key)`; make other `ID_CDROM_*` keys in-scope (cdrom_id capabilities reimplemented). Keep everything else unchanged.

## Verification

### Unit tests — `tests/test_cdrom_id.c`
- **sr0 capability fixture** (`tests/fixtures/cdrom_getconf_sr0.h`, the captured 384-byte GET CONFIGURATION): `cdrom_id_decode` it; assert exactly the 17 ground-truth keys are present, each `"1"`, and assert **no `ID_CDROM_MEDIA*`** key is emitted (this slice is capability-only).
- **empty/short blob:** `cdrom_id_decode` of a truncated/zero blob emits 0 keys (no crash, bounds-safe descriptor walk).
- **synthetic single-profile:** a hand-built blob with only profile 0x08 → `ID_CDROM` + `ID_CDROM_CD` and nothing else (isolates the table mapping).

### Live gate — `tests/verify_cdrom_id_live.sh` (sudo)
- `./udev-parity`: assert `VALUE MISMATCHES: 0` and `IN-SCOPE MISSING: 0`.
- Coldplug to the shadow db; **anti-hollow positive:** assert `b11:0` (sr0) carries `ID_CDROM=1` and at least `ID_CDROM_DVD_RAM=1` + `ID_CDROM_CD_RW=1` (proves the builtin fired and decoded real profiles).
- **Regression guard:** ATA disk `b8:0` still `ID_ATA=1`; usb disk `b8:48` still `ID_SERIAL=...-0:0`; video0 `c81:0` still `ID_V4L_CAPABILITIES=:capture:` (slices 3a/3b/3c intact).
- Note: cdrom_id must NOT run blkid on sr0 (blkid already excludes `sr*`); this slice does not change that.

### vmtest
Not PID 1 — boot rail must pass unchanged. `cd ~/schema-livetest && ./vmtest.sh`.

### Boundary
`schema-udev.c`, `schema-udev.h`, and the group-1 netlink bind stay byte-identical. Changes: `cdrom_id.h` (new), `udev_builtins.h`, `udev-parity.h`, `tests/` (unit + live + fixture), `Makefile`.

## Error handling
- `cdrom_get_config` open/ioctl failure → `cdrom_id_build` returns 0 keys; dispatch continues (a cdrom drive with a failing bus never crashes coldplug).
- Descriptor walk is bounds-checked (`4 + additional_length` never reads past `len`); a malformed blob yields whatever parsed cleanly, no overrun.
- No disc present is irrelevant — capabilities come from the drive, not the media.

## Corrections applied during review
- **Optical filesystem stays deferred to 3e (pare-back).** Greg's PR added a `fs_probe_iso9660` prober to `blkid_fs.h` and dropped the `sr*` exclusion from the `UB_BLKID` gate, so blkid ran on optical media. This was reverted: (1) it is media-dependent filesystem probing that belongs in 3e, not 3d capabilities; (2) it only covered ISO9660, not UDF; (3) **the real daemon does not reproduce it reliably** — cold optical reads during coldplug fail the raw read at LBA 16, so the daemon's shadow record lacked all `ID_FS_*` keys while the parity tool (reading the disc warm) reported 0 in-scope missing — a gate that did not reflect the daemon's actual output. Correct 3d boundary: `sr*` stays excluded from `UB_BLKID`, and `parity_is_optical(devpath)` defers `ID_FS_*` on optical devices (like `ID_CDROM_MEDIA_*`), so 3d passes honestly with any disc state. Greg's ISO9660 prober graduates to slice 3e, done with UDF + spin-up/media-ready reliability + a gate verifying the daemon's own output.
