# schema-udev sub-project B slice 3b — usb-storage block identity

**Status:** design approved 2026-08-07. Endgame arc: udevd retirement. Slice 3 (deferred builtins) decomposed into 3a ata_id (#91, merged), **3b usb-storage block identity** (this spec), 3c v4l_id. `cdrom_id`, `mtd_probe`, and true `scsi_id` (SG_IO INQUIRY for non-usb SAS/parallel SCSI) are deferred — no hardware on blakbox to parity-verify against.

## Goal

Bring **usb-storage block identity** in-scope and parity-verified, resolving the sdd (USB Seagate Expansion) identity that slice 1 deferred (the `-0:0` lun-suffixed `ID_SERIAL`, the `ID_USB_*` family, `ID_INSTANCE`). This is **not** a new builtin: everything udev emits for a usb-storage disk comes from `usb_id`, which already walks up to the usb_device and composes the scsi `-C:L` serial from sysfs sysattrs. The only gaps are that `ub_select` never fires `usb_id` on block nodes, and `usb_id` does not yet emit `ID_INSTANCE`/`ID_USB_INSTANCE`.

## Key finding (empirical, 2026-08-07)

Our system currently produces almost nothing for sdd (b8:48) — only the two hwdb `_FROM_DATABASE` keys. udev's full b8:48 identity set is **100% usb_id-reproducible**:
- Identity: `ID_BUS=usb`, `ID_MODEL(_ENC/_ID)`, `ID_VENDOR(_ENC/_ID)`, `ID_SERIAL`, `ID_SERIAL_SHORT`, `ID_REVISION`, `ID_TYPE`, `ID_INSTANCE`.
- `ID_USB_*` family: `ID_USB_MODEL(_ENC/_ID)`, `ID_USB_VENDOR(_ENC/_ID)`, `ID_USB_SERIAL(_SHORT)`, `ID_USB_REVISION`, `ID_USB_TYPE`, `ID_USB_INTERFACES`, `ID_USB_INTERFACE_NUM`, `ID_USB_DRIVER`, `ID_USB_INSTANCE`.
- `ID_PATH*` (path_id) and `ID_PART_TABLE_*` (blkid) are already handled.
- Ground truth: `ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0`, `ID_INSTANCE=0:0`, `ID_USB_INSTANCE=0:0`. No key requires a real SCSI INQUIRY — the kernel already exposes scsi `vendor`/`model`/`rev` in sysfs, which `usb_id`'s scsidir walk reads.

sdd has a partition **sdd1 (b8:49)** which udev gives the inherited usb-storage identity (`ID_BUS=usb`, `ID_MODEL`, `ID_SERIAL`, `ID_VENDOR`, `ID_INSTANCE=0:0`).

## Scope

### In scope (four changes, no new file)
1. **`udev_builtins.h` `ub_select`** — fire `UB_USB` on `SUBSYSTEM==block` + `DEVTYPE==disk` + a usb ancestor (via the existing `ub_ancestor_in` helper), in addition to the current `usb_device` gate. Mirrors udev's shipped `SUBSYSTEMS=="usb", IMPORT{builtin}="usb_id"` rule for usb-storage.
2. **`usb_id.h`** — emit `ID_INSTANCE` and `ID_USB_INSTANCE` = `C:L` (e.g. `0:0`) when the scsi address is parsed. `usb_id_build` already computes `C`,`L` for the `-C:L` serial suffix; capture it into an `instance[]` string and `UEMIT` both keys when non-empty.
3. **`udev-parity.h`** — block identity keys become in-scope on the **usb** chain as well as `/ata` (currently only `/ata`). Non-usb SCSI block stays deferred.
4. **`udev_rules.h` `rules_block_bypass`** — extend the ancestor-`ID_BUS=="ata"` identity-inheritance allowance to also `=="usb"`, so usb-storage partitions (sdd1) inherit the disk's identity. Same pattern as the slice-3a ata partition fix.

### Out of scope (deferred — no live target on blakbox)
- True `scsi_id` (SG_IO INQUIRY for parallel/SAS/non-usb SCSI). All blakbox disks are ata (sda/sdb/sdc) or usb (sdd); no real SCSI target exists to parity-verify against.
- `cdrom_id`, `mtd_probe` (slice-3 deferrals, unchanged).
- Group-2 rebroadcast, any socket change. Group-1 bind stays byte-identical.

## Mechanism (reuse, not reimplement)

`usb_id.h`'s `usb_find_nodes` walks **up** from any devpath to the usb_device ancestor; `usb_id_build`'s scsidir loop walks from the block devpath to that usb_device looking for a scsi_device node (one carrying a `vendor` sysattr), reads `vendor`/`model`/`rev` from sysfs, and composes `ID_SERIAL = <vendor>_<model>_<serial>-C:L`. Invoked on the block disk devpath, it therefore reproduces udev's b8:48 identity exactly. The kernel's scsi layer already performed the INQUIRY and published the results in sysfs — no ioctl needed.

### `ID_INSTANCE` addition (usb_id.h)
In the `if (scsidir[0])` block where `sscanf(pi_base(scsidir), "%u:%u:%u:%u", &H,&C,&T,&L)` runs, also record `instance = "C:L"`. In the `UEMIT` section emit `ID_INSTANCE` and `ID_USB_INSTANCE` = `instance` when set. This is additive and fires only on the scsi/usb-storage path — regular usb devices (no scsidir) emit neither, matching udev.

## Verification

### Unit test — `tests/test_usb_id.c` (extend, or add if absent)
`ID_INSTANCE` derives from sysfs topology (`scsidir` basename `H:C:T:L`), so test it with the synthetic-sysfs harness pattern already used by `test_udev_rules.c`/`test_udev_builtins.c`: build a fake usb_device → usb_interface → scsi_host → scsi target → scsi_device(`0:0:0:0`, with `vendor`/`model`/`rev` sysattrs) → block(`sdX`) tree, plus the usb_device's `idVendor`/`idProduct`/`serial` sysattrs, then assert `usb_id_build` on the block devpath emits `ID_INSTANCE=0:0`, `ID_USB_INSTANCE=0:0`, and `ID_SERIAL` ending `-0:0`. This is the honest unit; the live gate is the end-to-end authority.

### Live gate — `tests/verify_usb_storage_id_live.sh` (sudo)
- Run coldplug (daemon) + `./udev-parity`.
- Assert `VALUE MISMATCHES: 0` and `IN-SCOPE MISSING: 0` (the `-0:0` composed serial is now in-scope; a wrong composition shows as a value mismatch, a dispatch/gate miss as in-scope-missing).
- **Anti-hollow positive:** assert the usb disk sdd (b8:48) shadow record carries `ID_BUS=usb`, `ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0`, `ID_INSTANCE=0:0`; and the partition sdd1 (b8:49) inherits `ID_BUS=usb` + `ID_SERIAL` (proves both dispatch and partition inheritance fired).
- **Regression guard:** assert the 3 ATA disks (b8:0/16/32) still carry `ID_ATA=1` + `ID_BUS=ata` (slice-3a not disturbed).

### vmtest
Not PID 1 — boot rail must pass unchanged. `cd ~/schema-livetest && ./vmtest.sh`.

### Boundary
`schema-udev.c`, `schema-udev.h`, and the group-1 netlink bind stay byte-identical. Changes: `udev_builtins.h` (gate), `usb_id.h` (ID_INSTANCE), `udev-parity.h` (usb-chain in-scope), `udev_rules.h` (block_bypass usb allowance), `Makefile` (test target if a new test file is added).

## Error handling
- usb_id on a block disk with no usb ancestor never fires (gate excludes it); ata/nvme disks unaffected.
- `usb_id_build` returning -1 (not usb) on a mis-gated device → 0 keys absorbed, safe.
- Non-scsi usb device → no `instance` → neither instance key emitted (correct).

## Corrections applied during review
*(populated post-Greg, as in prior slices)*
