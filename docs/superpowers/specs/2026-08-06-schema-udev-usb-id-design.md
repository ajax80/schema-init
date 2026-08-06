# schema-udev `usb_id` builtin — design

**Date:** 2026-08-06
**Status:** approved
**Context:** Second builtin toward udevd retirement (Phase 3b cutover), after
`path_id` (builtin #1). Reproduces systemd-udev's USB identification
properties from the USB device-descriptor sysattrs. Same mechanism-only,
off-by-default boundary as path_id: new `usb_id.h`, `schema-udev.c` untouched.

## Goal

A pure C function that reproduces udev's USB `ID_*` / `ID_USB_*` properties for
a device, **byte-for-byte**, verified to **0 mismatches** across every device
blakbox's real udev assigned USB identity to.

## Scope

**blakbox-first, byte-parity.** The real usb_id device population is **41**
records (those carrying `E:ID_USB_VENDOR_ID=` in `/run/udev/data`). The parity
harness's "56" over-counted — it hinted on `ID_MODEL`/`ID_SERIAL`/`ID_VENDOR`,
which non-USB devices (ATA/SCSI disks via ata_id/scsi_id) also carry. The
acceptance gate targets the 41 true usb_id records.

usb_id owns exactly these keys (the parity filter includes only these; it must
exclude hwdb's `*_FROM_DATABASE` and path_id's `ID_PATH*`):

- Unprefixed: `ID_BUS`, `ID_VENDOR`, `ID_VENDOR_ENC`, `ID_VENDOR_ID`,
  `ID_MODEL`, `ID_MODEL_ENC`, `ID_MODEL_ID`, `ID_REVISION`, `ID_SERIAL`,
  `ID_SERIAL_SHORT`, `ID_TYPE`
- `ID_USB_` prefixed duplicate of each of the above, plus `ID_USB_INTERFACES`,
  `ID_USB_INTERFACE_NUM`, `ID_USB_DRIVER`

Note on `ID_TYPE` (unprefixed): on block devices it is later overwritten by
scsi/ata builtins, so it is contentious across builtins. The authoritative
usb_id signal is `ID_USB_TYPE`; the gate compares `ID_USB_TYPE` always, and
unprefixed `ID_TYPE` only on records where usb_id is the sole setter (no
`ID_USB_TYPE`≠`ID_TYPE` conflict).

## Architecture

`usb_id_build(const char *sysroot, const char *devpath, struct uevent *out)`
populates a caller-owned `struct uevent` with the usb_id properties (≤24 keys
observed max, within the existing `UE_MAX_KEYS`=32 — **no change to
`schema-udev.h`**). Internally:

1. **Locate the two anchor devices** from `devpath` (walk up the sysfs chain):
   - the **usb_device** (kernel name has no `:`, e.g. `1-4`, `usb1`) — source of
     all descriptor sysattrs.
   - the **usb_interface** (kernel name contains `:`, e.g. `1-4:1.0`), if the
     invoking device is on/under one — source of the interface-level props.
   A leaf like `.../1-4/1-4:1.0/video4linux/video1` climbs to `1-4:1.0` (first
   ancestor whose basename contains `:`) then to `1-4` (first ancestor whose
   basename has no `:` and is a usb node).
2. **Read descriptors** from the usb_device and apply the encoders + fallbacks.
3. **Enumerate interfaces** for `ID_USB_INTERFACES`.
4. **Emit** the unprefixed keys and their `ID_USB_` duplicates.

The function reads sysfs and writes nothing (same safety class as path_id).

## The two string encoders (the crux of byte-parity)

Both operate over the same **safe-char set**:

```
SAFE = A-Z a-z 0-9  #  +  -  .  :  =  @  _
```

Verified against live strings: `7.0.12-cachyos1-schema.fc44.x86_64` and
`0000:02:00.0` survive intact (`-`, `.`, `:` all kept); space and `,` do not.

### plain form — `ID_VENDOR`, `ID_MODEL`, `ID_SERIAL_SHORT`, and serial parts
Two passes, in order:
1. **replace_whitespace:** strip leading and trailing whitespace; replace each
   internal run of whitespace with a single `_`.
2. **replace_chars:** replace every remaining char not in SAFE with `_`.

Examples (ground truth):
- `"USB OPTICAL MOUSE "` → `USB_OPTICAL_MOUSE` (trailing space trimmed)
- `"GenesysLogic Technology Co., Ltd."` → `GenesysLogic_Technology_Co.__Ltd.`
  (spaces→`_`, `,`→`_`, `.` kept — note the `__` from `,` then space)
- `"Expansion       "` → `Expansion` (all-trailing whitespace trimmed)

### ENC form — `ID_VENDOR_ENC`, `ID_MODEL_ENC`
Single pass, **no trimming, no collapsing**: copy each char verbatim if in SAFE,
else emit it as `\xNN` (lowercase two-hex of the byte).

Examples (ground truth):
- `"GenesysLogic Technology Co., Ltd."` →
  `GenesysLogic\x20Technology\x20Co.\x2c\x20Ltd.`
- `"Expansion       "` → `Expansion\x20\x20\x20\x20\x20\x20\x20` (every trailing
  space escaped — the ENC form does **not** trim, unlike plain)
- `"Seagate "` → `Seagate\x20`

**The asymmetry is load-bearing:** plain trims+collapses, ENC escapes each byte.
A device with trailing spaces (Seagate, Expansion) proves the two forms diverge.

## Descriptor sources and fallbacks

From the usb_device sysfs dir:

| property | sysattr | fallback if sysattr absent |
|----------|---------|----------------------------|
| `ID_VENDOR_ID` | `idVendor` | (always present) |
| `ID_MODEL_ID` | `idProduct` | (always present) |
| `ID_REVISION` | `bcdDevice` | (always present) |
| `ID_VENDOR` / `_ENC` | `manufacturer` | the raw `idVendor` hex (e.g. `18f8`) |
| `ID_MODEL` / `_ENC` | `product` | the raw `idProduct` hex (e.g. `0f99`) |
| `ID_SERIAL_SHORT` | `serial` | omitted entirely (no key emitted) |

`ID_BUS` is the constant `usb`. The raw-hex fallback value is used for **both**
the plain and ENC form (e.g. no-manufacturer → `ID_VENDOR=18f8` and
`ID_VENDOR_ENC=18f8`).

## `ID_SERIAL` composition

```
ID_SERIAL = <ID_VENDOR> "_" <ID_MODEL> [ "_" <ID_SERIAL_SHORT> ]
```

Uses the already-computed plain forms (with fallbacks applied). The
`_<ID_SERIAL_SHORT>` suffix is appended only when a `serial` sysattr exists.

- No serial (camera): `GenesysLogic_Technology_Co.__Ltd._USB2.0_UVC_PC_Camera`
- No manufacturer/serial (mouse): `18f8_USB_OPTICAL_MOUSE`
- With serial (Pico Key): `Pol_Henarejos_Pico_Key_44BA59F930300000`

## `ID_TYPE` / `ID_USB_TYPE` — interface-class map

Mapped from the **invoked interface's** `bInterfaceClass` (hex string):

| class | type |
|-------|------|
| 01 | audio |
| 03 | hid |
| 06 | media |
| 07 | printer |
| 08 | *(mass storage — refine by `bInterfaceSubClass`)* |
| 09 | hub |
| 0e | video |
| e0 | wireless |
| ff | generic |
| *other* | generic |

Mass-storage (class 08) subclass refinement:

| subclass | type |
|----------|------|
| 02 | cd |
| 03 | tape |
| 04, 07 | floppy |
| 06 | disk |
| *other* | disk |

Verifiable on blakbox: audio, hid, video, disk (USB stick, class 08 subclass 06).
The rest are included per udev's map for correctness (untested here).

## `ID_USB_INTERFACES`

The `:`-wrapped, `:`-delimited list of 6-hex interface triplets
(`bInterfaceClass` + `bInterfaceSubClass` + `bInterfaceProtocol`, `%02x%02x%02x`
each), built by iterating interfaces in **ascending `bInterfaceNumber` order**
and **deduplicating** identical triplets (keep first occurrence).

- Camera (ifaces 0,1 = 0e0100, 0e0200): `:0e0100:0e0200:`
- Mouse (ifaces 0,1 = 030102, 030101 — *not* value-sorted): `:030102:030101:`
- Pico Key (ifaces 0,1,2,3 = 030000, 030000, 0b0000, ff0000 — dup collapsed):
  `:030000:0b0000:ff0000:`

Enumerate by scanning the usb_device's child dirs matching `<usbdev>:*` (the
interface dirs), sort by `bInterfaceNumber`, format, dedup.

## `ID_USB_INTERFACE_NUM` and `ID_USB_DRIVER`

From the **invoked interface** dir (when present):
- `ID_USB_INTERFACE_NUM` = `bInterfaceNumber` sysattr (2 hex, e.g. `00`, `01`).
- `ID_USB_DRIVER` = basename of the interface's `driver` symlink (e.g.
  `uvcvideo`, `usbhid`).

Omitted when the invoking device is not on/under an interface (bare usb_device).

## Output shape

`usb_id_build` fills `out` (a caller-provided `struct uevent`) via `safe_copy`
into `out->key[]` / `out->val[]`, incrementing `out->n`. It emits the unprefixed
keys first, then the `ID_USB_`-prefixed duplicates, then `ID_USB_INTERFACES` /
`ID_USB_INTERFACE_NUM` / `ID_USB_DRIVER`. Returns 0 on success, −1 if the device
is not a USB device (no `idVendor` reachable).

## Files

- **Create `usb_id.h`** — the builtin: the two encoders (`usb_replace_whitespace`,
  `usb_replace_chars`, `usb_encode`), descriptor reader, interface enumeration,
  the type map, and `usb_id_build`. Includes `schema-udev.h` for `struct uevent`
  / `safe_copy`. A new sibling header, same rationale as `path_id.h`.
- **Create `tests/test_usb_id.c`** — unit tests: the encoders against the exact
  ground-truth strings above (both forms, including the trailing-whitespace
  asymmetry), the fallbacks, serial composition, the type map (incl. class-08
  subclass), and `ID_USB_INTERFACES` ordering+dedup — driven over a synthetic
  usb sysfs tree built under `mkdtemp` (the `test_coldplug.c` idiom).
- **Create `tests/verify_usb_id_live.sh`** — live acceptance gate.
- **Modify `Makefile`** — one `test:` line for `test_usb_id.c`.
- **`schema-udev.c` — untouched.** Boundary gate: `git diff master..HEAD --
  schema-udev.c` empty; `grep usb_id schema-udev.c` empty.

## Error handling

- Not a USB device (no reachable `idVendor`) → `usb_id_build` returns −1, emits
  nothing.
- Missing optional sysattr (`manufacturer`, `product`, `serial`) → apply the
  fallback / omit per the table; never fail the whole build.
- Never write sysfs, never emit netlink, never touch `/run/udev`.

## Testing

**Unit (deterministic, CI):** `tests/test_usb_id.c` asserts, at minimum:
1. `usb_replace_whitespace("USB OPTICAL MOUSE ")` → `USB_OPTICAL_MOUSE`
2. full plain pipeline (replace_whitespace then replace_chars) of
   `"GenesysLogic Technology Co., Ltd."` → `GenesysLogic_Technology_Co.__Ltd.`
   (space→`_`, comma→`_`, `.` kept)
3. `usb_encode("GenesysLogic Technology Co., Ltd.")` →
   `GenesysLogic\x20Technology\x20Co.\x2c\x20Ltd.`
4. `usb_encode("Seagate ")` → `Seagate\x20` (ENC no-trim)
5. plain of `"Expansion       "` → `Expansion` (trim) vs ENC → 7×`\x20`
6. no-manufacturer fallback → `ID_VENDOR` = idVendor hex, both forms
7. serial composition with and without serial
8. type map: class 0e→video, 03→hid, 01→audio, 08/06→disk
9. `ID_USB_INTERFACES` for mouse (`:030102:030101:`) and pico
   (`:030000:0b0000:ff0000:`, dedup)
10. full synthetic device → complete key set matches

**Acceptance (live, Claire post-Greg):** `tests/verify_usb_id_live.sh` runs
`usb_id_build` over every device with `E:ID_USB_VENDOR_ID` in `/run/udev/data`
(the 41), and diffs the emitted usb_id-owned keys against the stored `E:` props.
**Require 0 mismatches across all 41 devices.**

**Boundary + vmtest** as with path_id.

## Out of scope

- Wiring usb_id into the live daemon (deferred cutover).
- `ID_VENDOR_FROM_DATABASE` / `ID_MODEL_FROM_DATABASE` — those are hwdb, a
  separate builtin.
- Non-USB `ID_TYPE` (ATA/SCSI disks) — owned by ata_id/scsi builtins.
- Parsing the binary `descriptors` file — sysfs interface-dir enumeration
  (sorted + deduped) reproduces `ID_USB_INTERFACES` for blakbox's population;
  the binary descriptors file is the only fully-faithful source and can be
  revisited if a device ever orders interfaces non-monotonically.
