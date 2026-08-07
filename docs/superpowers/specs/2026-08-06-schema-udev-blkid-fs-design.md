# schema-udev builtin #5 (PR-B): blkid filesystem probers — Design

**Status:** approved 2026-08-06
**Builtin:** #5 of the udevd-retirement cutover (Phase 3b). blkid PR-A (partition tables) merged as #85. **This is PR-B: filesystem superblock probers** (`ID_FS_*`).
**Boundary:** mechanism-only, off by default, `schema-udev.c` / `schema-udev.h` byte-identical. Wired to nothing live.

## Goal

Reproduce, byte-for-byte, the **identity** `ID_FS_*` properties that systemd-udevd's `blkid` builtin
synthesizes — by reading the raw block device superblock of six filesystems: **ext4, btrfs, vfat,
ntfs, exfat, swap**. Acceptance is **0 mismatches vs real udev across the 11 formatted nodes on
blakbox** (10 partition filesystems + swap), both directions, on the identity-field subset.

## Field scope (Jonathan's decision: identity fields)

**Implemented** (per FS as applicable):
`ID_FS_TYPE`, `ID_FS_USAGE`, `ID_FS_UUID`, `ID_FS_UUID_ENC`, `ID_FS_LABEL`, `ID_FS_LABEL_ENC`,
`ID_FS_UUID_SUB`, `ID_FS_UUID_SUB_ENC`, `ID_FS_VERSION`.

**Deferred** (excluded from the live gate BOTH directions, like net_id's `ID_NET_DRIVER`):
`ID_FS_SIZE`, `ID_FS_BLOCKSIZE`, `ID_FS_LASTBLOCK` — informational; exact libblkid per-FS size math
is a possible PR-B2. The identity fields are what `/dev/disk/by-uuid`, `by-label`, and `mount -U`
consume.

## Normative reference

Faithful port of systemd v259 `udev-builtin-blkid.c` + util-linux libblkid `superblocks/{ext,btrfs,
vfat,ntfs,exfat,swap}.c`. Source governs on disagreement; the live parity gate is the authority.

## Reuses PR-A

`blkid_fs.h` includes `blkid_pt.h` (on master) to reuse `bpt_read_at` (raw `pread`, read-only),
`bpt_le16/32/64`, `bpt_emit`. **FS UUIDs are straight-byte** — a NEW `fs_uuid_straight` formatter,
NOT PR-A's `bpt_guid_str` (which reverses the first three GPT groups). Verified: ext4 UUID bytes
`cf 4f 2b 07 …` → `cf4f2b07-…` (straight).

## Verified superblock layout (read from blakbox raw devices)

| FS | magic | UUID | UUID_SUB | LABEL | VERSION | USAGE |
|---|---|---|---|---|---|---|
| **ext4** | `0x53EF` (LE) at sb+56, sb@1024 | straight 16B @ sb+104 | — | 16B @ sb+120 (NUL-term) | `%u.%u` of `s_rev_level`(u32@sb+0x4C) `.` `s_minor_rev_level`(u16@sb+0x7E) = `1.0` | filesystem |
| **btrfs** | `_BHRfS_M` at sb+64, sb@0x10000 | fsid straight 16B @ sb+32 | dev_item.uuid straight 16B @ **sb+267** | 256B @ sb+299 | — | filesystem |
| **vfat** | boot sig `0x55AA`@510 + BPB checks | FAT32 serial 4B @ 67 → `%02X%02X-%02X%02X`(b3,b2,b1,b0) | — | 11B @ 71 (FAT32); `NO NAME    ` ⇒ none | `FAT32`/`FAT16`/`FAT12` | filesystem |
| **ntfs** | `NTFS    ` at 3 | serial 8B @ 72 → 16 hex UPPER, byte-reversed (b7..b0) | — | MFT `$Volume` (record 3) `$VOLUME_NAME` attr, UTF-16LE | — | filesystem |
| **exfat** | `EXFAT   ` at 3 | serial 4B (VBR) → `%04X-%04X` | — | root-dir label entry (UTF-16LE) | — | filesystem |
| **swap** | `SWAPSPACE2`/`SWAP-SPACE` @ pagesize−10 | straight 16B @ 1036 | — | 16B @ 1052 | `%u` of version(u32@1024) = `1` | **other** |

Verified real vectors:
- ext4 nvme0n1p2: UUID `cf4f2b07-f150-404f-bde1-4d9f545594b4`, no label.
- btrfs nvme0n1p3: UUID `90557be5-57a8-4ff5-bc32-e1bc83be6d75`, UUID_SUB `94ecd0f5-5b70-413e-b71f-dd6760668f32`, LABEL `fedora`.
- vfat nvme0n1p1: serial `7c 76 73 07` → `0773-767C`; label `NO NAME    ` ⇒ no `ID_FS_LABEL`.
- ntfs sdb2: serial `33 48 84 54 7b 84 54 6e` → `6E54847B54844833`; no label (sdb4 = `RECOVERY`).
- swap zram0: UUID `c2e50da1-0f93-4f2b-8132-29e314f2c827`, version `1`, USAGE `other`, no label.

## Encoding

- **`ID_FS_UUID` / `ID_FS_LABEL`** = libblkid `blkid_safe_string` (control chars, `/`, and 0x7f →
  `_`; spaces and printable kept).
- **`ID_FS_UUID_ENC` / `ID_FS_LABEL_ENC`** = libblkid `blkid_encode_string` (every byte not in
  `[0-9A-Za-z]` or `#+-.:=@_` → `\xNN`, as in PR-A's name encoder).
- Blakbox labels are ASCII alnum → both forms coincide here, but both are ported for dongles.
- ntfs/exfat labels are UTF-16LE → decode to UTF-8 first, then apply the two encoders.

## Algorithm

`blkid_fs_build(sysroot, devpath, devnode, out)`:
1. Probe each FS in libblkid order until one matches its magic; on match, emit that FS's fields and
   return (no fall-through — first match wins).
   Order: **btrfs → ext → vfat → ntfs → exfat → swap** (magics are distinct; ext/vfat/ntfs checked
   with their specific signatures to avoid cross-detection).
2. Each prober reads only the bytes it needs via `bpt_read_at`; emits `ID_FS_TYPE`, `ID_FS_USAGE`,
   then UUID/LABEL/VERSION/SUB as available. **`ID_FS_LABEL`/`_ENC` only if the label is non-empty**
   (and not the vfat `NO NAME    ` sentinel). `ID_FS_UUID_SUB` only for btrfs.
3. No match → emit nothing (whole-disk nodes, empty partitions, unformatted devices).

`sysroot`/`devpath` are unused for the FS probe itself (kept for signature symmetry with the other
builtins and possible future sysfs cross-checks); the probe works off `devnode`.

## Components

- **`blkid_fs.h`** (new): includes `blkid_pt.h`. Public `int blkid_fs_build(const char *sysroot,
  const char *devpath, const char *devnode, struct uevent *out)`. Internals: `fs_uuid_straight`,
  `fs_uuid_hex` (vfat/ntfs/exfat serial forms), `fs_safe_bytes` (blkid_safe_string),
  `fs_encode_bytes` (blkid_encode_string), `fs_utf16_to_utf8`, `fs_emit_label` (emits LABEL+_ENC if
  non-empty), `fs_emit_uuid` (emits UUID+_ENC), and one prober per FS
  (`fs_probe_ext`/`_btrfs`/`_vfat`/`_ntfs`/`_exfat`/`_swap`).
- **`tests/test_blkid_fs.c`** (new): fabricated superblock per FS in a tmpfile using the verified
  real vectors (ext4 cf4f2b07 no-label; btrfs fsid+sub+`fedora`; vfat `0773-767C`+`NO NAME` empty;
  ntfs serial→`6E54847B54844833` + a synthetic MFT with `RECOVERY`; swap uuid+`other`; exfat
  serial+label). Assert exact emitted key set. Plus unit vectors for `fs_uuid_straight`,
  `fs_encode_bytes` (space→`\x20`), `fs_utf16_to_utf8`.
- **`tests/verify_blkid_fs_live.sh`** (new): for every `/sys/class/block/*` node, run the driver
  (**under `sudo`**), diff the identity `ID_FS_*` subset of `udevadm info` both directions —
  **excluding `ID_FS_SIZE`/`ID_FS_BLOCKSIZE`/`ID_FS_LASTBLOCK`** from both sides. Expect **11
  formatted nodes (10 fs + swap), 0 mismatches** (whole-disk/table-only nodes contribute nothing).
- **`Makefile`**: one line to build+run `tests/test_blkid_fs.c`.
- **`schema-udev.c` / `schema-udev.h`**: unchanged (byte-identical).

## Testing / acceptance gate

1. `make test` green incl. `test_blkid_fs`, `-Wall -Wextra` clean.
2. Boundary: `git diff master -- schema-udev.c schema-udev.h` empty; `grep blkid_fs schema-udev.c`
   empty.
3. Live: `tests/verify_blkid_fs_live.sh` → **0 mismatches** across the 11 formatted nodes, both
   directions (identity subset).
4. `vmtest.sh` → RESULT: PASS.

## Out of scope

- `ID_FS_SIZE` / `ID_FS_BLOCKSIZE` / `ID_FS_LASTBLOCK` (deferred; possible PR-B2 with per-FS size math).
- Any filesystem beyond the six (xfs, f2fs, LUKS, LVM, mdraid, iso9660, hfs+, …).
- All `ID_PART_*` (PR-A).
- Any live wiring — `schema-udev.c` stays byte-identical.

## Notable risk

**NTFS label = MFT walk** — the only prober that is not a fixed-offset read: boot sector →
bytes-per-sector (u16@11) × sectors-per-cluster (u8@13) = cluster size; MFT LCN (s64@48) → MFT byte
offset; MFT record size (s8@64: if <0, `1 << -val`, else `× cluster`); read record 3 (`$Volume`),
apply the fixup array, walk attributes to `$VOLUME_NAME` (type `0x60`), decode its UTF-16LE value.
Live vectors: sdb4 `RECOVERY`, sdb2 (no label). Port from libblkid `superblocks/ntfs.c`.
