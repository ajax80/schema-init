# schema-udev builtin #5 (PR-A): blkid partition tables — Design

**Status:** approved 2026-08-06
**Builtin:** #5 of the udevd-retirement cutover (Phase 3b), after path_id (#1), usb_id (#2), input_id (#3), net_id (#4). blkid is split into **PR-A (this doc): partition tables + entries** and **PR-B: filesystem superblock probers** (`ID_FS_*`, separate spec).
**Boundary:** mechanism-only, off by default, `schema-udev.c` / `schema-udev.h` byte-identical. Wired to nothing live.

## Goal

Reproduce, byte-for-byte, the `ID_PART_TABLE_*` and `ID_PART_ENTRY_*` properties that
systemd-udevd's `blkid` builtin synthesizes — by reading the **raw block device** and parsing the
GPT (and MBR) partition table on-disk structures. Acceptance is **0 mismatches vs real udev across
the ~15 GPT nodes on blakbox** (5 disks + 10 partitions; count varies with what's attached), both
directions, with GPT fully implemented and MBR/dos ported + unit-tested (no MBR disk here).

## New capability: raw block-device reads

This is the **first builtin to read a raw block device** (`open(devnode, O_RDONLY)` + `pread`).
path_id / usb_id / input_id / net_id were pure sysfs; blkid is inherently a device prober. It stays
strictly read-only. Because block devices are `root:disk 0660` and the invoking user is not in the
`disk` group, the live gate driver **runs under `sudo`** (the one harness that needs it).

## Normative reference

Faithful port of systemd v259 `src/udev/udev-builtin-blkid.c` partition handling + the GPT/MBR
on-disk layout (UEFI spec / libblkid `partitions/gpt.c`, `partitions/dos.c`). Where this document
and the source/spec disagree, **the source governs and the live parity gate is the authority.**

## What PR-A emits — and what it does NOT

**Whole disk** (no sysfs `partition` file): emits ONLY
- `ID_PART_TABLE_TYPE` (`gpt` / `dos`)
- `ID_PART_TABLE_UUID` (GPT disk GUID / MBR 4-byte disk signature)

**Partition** (sysfs `partition` = N): emits the parent table's `ID_PART_TABLE_TYPE`/`UUID`
(inherited) **plus** this partition's entry:
- `ID_PART_ENTRY_SCHEME` (`gpt`/`dos`), `ID_PART_ENTRY_NUMBER` (N),
  `ID_PART_ENTRY_OFFSET` (starting LBA, 512-byte sectors), `ID_PART_ENTRY_SIZE`
  (sector count), `ID_PART_ENTRY_DISK` (parent `major:minor`)
- `ID_PART_ENTRY_UUID` (GPT unique GUID), `ID_PART_ENTRY_TYPE` (GPT type GUID)
- `ID_PART_ENTRY_NAME` — **only if the GPT name is non-empty** (UTF-16LE→UTF-8, blkid-encoded)
- `ID_PART_ENTRY_FLAGS` — **only if attributes ≠ 0** (`0x%016llx`)

PR-A does **NOT** emit any `ID_FS_*` (PR-B), and does not touch zram0 or any device without a
recognized table.

## Ground truth (blakbox, verified against raw disk + `udevadm info`)

The 16 GPT nodes and the emission buckets:

| node | kind | expected keys |
|---|---|---|
| nvme0n1, sda, sdb, sdc, sdd | whole disk | `ID_PART_TABLE_TYPE=gpt` + `ID_PART_TABLE_UUID=<disk GUID>` |
| nvme0n1p1 | partition | TABLE_* + ENTRY_{SCHEME,NAME="EFI System Partition",UUID,TYPE,NUMBER,OFFSET,SIZE,DISK} (no FLAGS) |
| nvme0n1p2 | partition | TABLE_* + ENTRY_* with **no NAME, no FLAGS** |
| sdb1, sdb2, sdb4 | partition | TABLE_* + ENTRY_* **with FLAGS** (`0x8000000000000000` / `0x1`) + NAME |
| sda1, sdc1, sdd1, nvme0n1p3 | partition | TABLE_* + ENTRY_* (NAME present except nvme0n1p3/sdd1/sdc?; per live) |
| zram0 | (excluded) | no table → nothing |

Verified decode of nvme0n1 GPT header (LBA1) and entry 1:
- disk GUID bytes `a6 6d d4 56 84 c4 d7 4d a6 c3 d4 69 3c 92 f9 4d` → `56d46da6-c484-4dd7-a6c3-d4693c92f94d`.
- entry-array LBA = 2, count = 128, entry size = 128.
- entry 1: type GUID `28 73 2a c1 1f f8 d2 11 ba 4b 00 a0 c9 3e c9 3b` → `c12a7328-f81f-11d2-ba4b-00a0c93ec93b`; unique GUID → `97a84a88-34e4-4dde-b2e8-4c1ee3d5fccc`; first_lba `0x800` = 2048 (OFFSET); last_lba `0x12c7ff` → SIZE = last−first+1 = 1228800; attrs 0 (no FLAGS); name UTF-16LE `EFI System Partition`.

## On-disk layout (exact — verified)

### GPT header (at LBA1 = byte offset 512, or `sector_size` if 4Kn — see note)
| offset | field | use |
|---|---|---|
| 0 | signature `"EFI PART"` (8 bytes) | GPT detection |
| 56 | disk GUID (16 bytes, mixed-endian) | `ID_PART_TABLE_UUID` |
| 72 | partition_entry_lba (u64 LE) | where entries start |
| 80 | num_partition_entries (u32 LE) | entry count |
| 84 | size_of_partition_entry (u32 LE) | entry stride |

### GPT partition entry (stride = size_of_partition_entry, typically 128)
| offset | field | use |
|---|---|---|
| 0 | type GUID (16, mixed-endian) | `ID_PART_ENTRY_TYPE` (all-zero ⇒ empty slot, skip) |
| 16 | unique GUID (16, mixed-endian) | `ID_PART_ENTRY_UUID` |
| 32 | first_lba (u64 LE) | `ID_PART_ENTRY_OFFSET` |
| 40 | last_lba (u64 LE) | `ID_PART_ENTRY_SIZE = last − first + 1` |
| 48 | attributes (u64 LE) | `ID_PART_ENTRY_FLAGS` (only if ≠ 0) |
| 56 | name (72 bytes UTF-16LE) | `ID_PART_ENTRY_NAME` (only if non-empty) |

**GUID mixed-endian:** first 3 groups little-endian (4/2/2 bytes), last 2 big-endian (2/6 bytes) →
`xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`, lowercase.

**Sector size:** blakbox is 512-byte (GPT header at 512). Use logical sector size from sysfs
`<disk>/queue/logical_block_size` (fallback 512); GPT header at LBA1 = `sector_size`. A 4Kn disk
puts the header at 4096. OFFSET/SIZE stay in **512-byte** units to match libblkid/live (nvme p1
OFFSET=2048 = first_lba regardless of sector size — GPT LBAs are logical; libblkid reports in
512-byte sectors, so scale `first_lba × (sector_size/512)` only if sector_size ≠ 512). On this box
sector_size = 512 so the scale factor is 1; the ×(sector_size/512) is unit-tested for a 4Kn tree.

### MBR/dos (unit-tested only — no MBR disk here)
- signature `0x55AA` at byte 510.
- `ID_PART_TABLE_UUID` = 4-byte disk signature at offset 440 (`%08x`, LE→hex); `TYPE=dos`.
- 4 primary entries at offset 446, stride 16: status(1, `0x80`⇒bootable→FLAGS `0x80`), type(1 →
  `ID_PART_ENTRY_TYPE=0x%02x`), first_lba (u32 LE offset 8 → OFFSET), num_sectors (u32 LE offset
  12 → SIZE). SCHEME=dos, NUMBER=index (1-4). Extended (type `0x05`/`0x0f`/`0x85`) → walk the
  logical chain, NUMBER starts at 5. No NAME/UUID for dos entries.

## Algorithm

`blkid_pt_build(sysroot, devpath, devnode, out)`:
1. Read sysfs `<sysroot><devpath>/partition`. Present ⇒ **partition N**; absent ⇒ **whole disk**.
2. **Whole disk:** `probe_table(devnode)` → if GPT, emit `ID_PART_TABLE_TYPE=gpt` +
   `ID_PART_TABLE_UUID`; elif MBR, emit `dos` + signature. Return.
3. **Partition:** parent devdir = `pi_parent(copy of sysroot+devpath)`; parent devnode =
   `/dev/<pi_base(parentdir)>`; parent `major:minor` from `pi_sysattr(parentdir,"dev")`.
   `probe_table(parent devnode)`:
   - emit inherited `ID_PART_TABLE_TYPE`/`UUID`.
   - locate entry N (GPT: entry at `part_entry_lba*sector + (N-1)*entry_size`; dos: primary index
     N or logical chain). Skip if the type GUID is all-zero.
   - emit `ID_PART_ENTRY_SCHEME/NUMBER/OFFSET/SIZE/DISK/UUID/TYPE`, conditional `NAME`/`FLAGS`.

`probe_table` opens the device once with `pread` (no full-device read; only header + the needed
entry). No CRC validation / backup-header fallback in PR-A (blakbox primaries are valid; deferred,
source governs).

## Components

- **`blkid_pt.h`** (new): includes `path_id.h`. Public `int blkid_pt_build(const char *sysroot,
  const char *devpath, const char *devnode, struct uevent *out)`. Internals: `bpt_emit`
  (UE_MAX_KEYS-guarded), `bpt_read_at` (`pread` wrapper), `bpt_guid_str` (mixed-endian→canonical),
  `bpt_gpt_disk_uuid`, `bpt_gpt_entry`, `bpt_name_encode` (UTF-16LE→UTF-8 + blkid-encode),
  `bpt_probe_mbr`, `bpt_is_partition`.
- **`tests/test_blkid_pt.c`** (new): fabricated GPT + MBR images in a tmpfile — whole-disk GPT
  (assert TABLE_TYPE/UUID), GPT partition with name+no-flags (the verified nvme p1 vector:
  TYPE=`c12a7328-…`, UUID=`97a84a88-…`, NAME=`EFI System Partition`, OFFSET=2048, SIZE=1228800),
  GPT partition with flags+no-name, empty-slot skip, `bpt_guid_str` unit vector, 4Kn sector scaling,
  MBR primary + bootable-flag + logical partition.
- **`tests/verify_blkid_pt_live.sh`** (new): for every `/sys/class/block/*` node, run the driver
  (**under `sudo`**), diff the `ID_PART_TABLE_*`/`ID_PART_ENTRY_*` subset of `udevadm info` both
  directions. Expect **~15 devices with a table, 0 mismatches** (zram0 and any table-less node
  contribute nothing on both sides; only `0 mismatches` gates).
- **`Makefile`**: one line to build+run `tests/test_blkid_pt.c`.
- **`schema-udev.c` / `schema-udev.h`**: unchanged (byte-identical).

## Testing / acceptance gate

1. `make test` green incl. `test_blkid_pt`, `-Wall -Wextra` clean.
2. Boundary: `git diff master -- schema-udev.c schema-udev.h` empty; `grep blkid_pt schema-udev.c`
   empty.
3. Live: `tests/verify_blkid_pt_live.sh` → **0 mismatches** across the 16 GPT nodes, both directions.
4. `vmtest.sh` → RESULT: PASS.

## Out of scope

- All `ID_FS_*` (PR-B: vfat/ext4/btrfs/ntfs/swap/exfat superblock probers).
- Non-GPT/MBR tables (BSD disklabel, Sun, Mac, atari, etc.).
- GPT CRC validation + backup-header fallback (deferred; source governs if a future device needs it).
- Any live wiring — `schema-udev.c` stays byte-identical.
