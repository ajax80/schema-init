# schema-udev `path_id` builtin — design

**Date:** 2026-08-06
**Status:** approved
**Context:** First builtin toward udevd retirement (Phase 3b cutover). The parity
harness (PR #79) produced a data-driven worklist ranked by device count;
`path_id` is #1 at 173–182 devices on blakbox. This builtin synthesizes the
`ID_PATH` / `ID_PATH_TAG` properties in-process so schema-udev's device
property set reaches parity with real systemd-udevd for the largest device
class.

## Goal

A pure C function that reproduces udev's `ID_PATH` (and its derived
`ID_PATH_TAG`) for a device, **byte-for-byte**, by walking the device's sysfs
ancestry. Verified to **0 mismatches** across every device blakbox's real udev
assigned an `ID_PATH`.

## Scope

**blakbox-first, byte-parity.** Implement exactly the six bus handlers the live
device population requires. Real udev's `path_id` implements ~15 bus types
(bcma, ccw, xen, virtio, sas, serio, sdio, ap, scm…); none of those hardware
buses exist on blakbox, so they are **out of scope**. The design keeps handler
dispatch open so a seventh handler drops in later (e.g. `mmc` for the ARM Pi
fleet) once we capture that hardware's parity data.

### Ground-truth device population (blakbox, 2026-08-06)

182 `/run/udev/data` records carry `E:ID_PATH=`. Every value decomposes into
these six handler tokens:

| handler   | appearances | example component            |
|-----------|-------------|------------------------------|
| pci       | 161         | `pci-0000:02:00.0`           |
| usb       | 69          | `usb-0:4:1.0`                |
| platform  | 21          | `platform-AMDI0030:00`       |
| ata       | 9           | `ata-1.0`                    |
| nvme      | 4           | `nvme-1`                     |
| scsi      | 2           | `scsi-0:0:0:0`               |

## Architecture — the core walk

Input is `(sysroot, DEVPATH)` where `DEVPATH` is the kernel device path
(`/devices/pci0000:00/...`) and `sysroot` is the sysfs mount (`/sys`, or a
`/tmp` fixture root in tests).

Start at the leaf device directory (`sysroot + DEVPATH`) and walk parent
directories root-ward. At each node, read its `subsystem` (the basename of the
`subsystem` symlink) and dispatch to a handler. A handler **prepends** a path
component to the accumulator and returns the node the walk should continue
from — some handlers *consume* a run of same-subsystem ancestors (pci bridges,
the usb host, the scsi/ata host), others just step to the immediate parent.
Because components are prepended (leaf-discovered first, but written to the
front), the finished string reads root→leaf.

This is the same control shape as systemd's `path_id.c` and reuses the
sysfs-reading idiom already established by `uevent_from_sysfs` in
`schema-udev.h`. **The function reads sysfs and writes nothing.**

### Anchor guard

Emit `ID_PATH` only if the walk reached a **pci or platform anchor** (i.e. at
least one pci/platform component was produced). Devices with no recognized
ancestry return −1 and get no `ID_PATH` — matching udev, which suppresses paths
it cannot anchor. A bare PCI function with no transport (e.g. the GPU at
`pci-0000:07:00.0`) is still emitted, because pci alone is a valid anchor.

## The six handlers (exact formats, ground-truth-verified)

All formats below were reverse-engineered against blakbox's live sysfs and its
real `/run/udev/data`, not from memory. Each row names the exact source field.

### pci
- **Component:** `pci-<sysname>` where sysname is the kernel dir name, e.g.
  `0000:02:00.0`.
- **Consume:** after emitting, skip **all** pci ancestors (PCI-to-PCI bridges
  are never emitted). Only the leaf-most pci function on the path appears.
- **Anchor:** yes.
- Examples: `pci-0000:07:00.0` (bare), `pci-0000:08:00.3-usb-…` (as prefix).

### usb
- Fires only when the node's `DEVTYPE` is `usb_interface` (kernel name like
  `1-4:1.0`). Non-interface usb nodes (usb_device, usb host) step to parent
  without emitting.
- **Component:** `usb-0:<rest>` where `<rest>` is the interface sysname after
  the first `-`. `1-4:1.0` → `4:1.0` → `usb-0:4:1.0`. The busnum before the `-`
  is dropped; the leading `0:` is a literal constant (verified constant across
  busnums 1–4 on this box).
- **Consume:** skip to the usb host controller's parent.
- Examples: `usb-0:4:1.0`, `usb-0:1:1.0`.

### scsi — ata transport
When a scsi_device sits under an ATA host (ahci), emit an `ata-` component
instead of a `scsi-` one.
- **Component:** `ata-<port_no>.<M>`.
  - `<port_no>`: read the `port_no` **sysattr** of the `ata_port` device
    (`/sys/class/ata_port/ataN/port_no`). **Do not parse the `ataN` dir name** —
    they can diverge (blakbox `ata9` has `port_no=1`). For blakbox's disks
    ata1/ata2/ata6 the port_no values are 1/2/6.
  - `<M>`: the ata_device number, the suffix after the dot in its kernel name
    `devN.M` (`dev1.0` → `0`). 0 for all non-port-multiplier disks on blakbox.
- **Consume:** skip to the ata port's parent (the pci function).
- Examples: `pci-0000:02:00.1-ata-1.0`, `…-ata-6.0`.

### scsi — default transport
- **Component:** `scsi-<H>:<C>:<T>:<L>` from the scsi_device sysname
  (`H:C:T:L`), **with H rebased**: subtract the lowest `hostN` index among the
  scsi_host's sibling directories (hosts sharing the same parent). On blakbox
  the USB-storage interface has a single `host9`, so H rebases 9 → 0, giving
  `scsi-0:0:0:0`.
- **Consume:** skip to the scsi host's parent.
- Example (nested): `pci-0000:08:00.3-usb-0:1:1.0-scsi-0:0:0:0`.

### nvme
- **Component:** `nvme-<nsid>` where `<nsid>` is the `nsid` sysattr of the
  **leaf block device** (`/sys/block/nvme0n1/nsid` → `1`).
- **Consume:** step to parent; the pci ancestor supplies the prefix.
- Example: `pci-0000:01:00.0-nvme-1`.

### platform
- **Component:** `platform-<sysname>` where sysname is the platform device's
  kernel dir name.
- **Anchor:** yes.
- **Consume:** step to immediate parent.
- Examples: `platform-AMDI0030:00`, `platform-serial8250`,
  `pci-0000:00:14.3-platform-PNP0800:00` (platform device under an LPC bridge).

## `ID_PATH_TAG`

Derived from `ID_PATH` by replacing every character **not** in `[A-Za-z0-9]`
with `_`. `-`, `:`, `.` all become `_`. Example:
`pci-0000:00:00.0` → `pci-0000_00_00_0`.

## Interface

```c
/* Build ID_PATH for the device at sysroot+devpath. Returns the ID_PATH
 * string length written to out (excluding NUL), or -1 if no path could be
 * anchored (unrecognized ancestry) or on buffer/read error. Reads sysfs,
 * writes nothing. */
ssize_t path_id_build(const char *sysroot, const char *devpath,
                      char *out, size_t outsz);

/* Transform an ID_PATH into its ID_PATH_TAG (non-alnum -> '_').
 * Returns 0, or -1 on buffer overflow. */
int path_id_tag(const char *id_path, char *out, size_t outsz);
```

Internal static helpers (all header-inline, sysfs-reading, no writes):
- read a node's subsystem basename (from its `subsystem` symlink)
- read a sysattr value (single-line file read, trimmed)
- climb to the parent device directory of a `/sys/devices/...` path
- per-handler component builders

## Files

- **Create `path_id.h`** — the builtin (functions above + static helpers).
  A new header rather than an addition to `schema-udev.h` because that file is
  already 408 dense lines spanning six unrelated jobs (uevent parse, rules,
  symlinks, coldplug, libudev frame, udev db), path_id is ~150 lines of bus
  logic on its own, and five more builtins (`net_id`, `usb_id`, `input_id`,
  `blkid`, `hwdb`) will land in this same slot. Each builtin as its own header
  stays independently readable and testable. `path_id.h` includes
  `schema-udev.h` for `struct uevent` / `safe_copy` reuse.
- **Create `tests/test_path_id.c`** — unit tests over captured real sysfs
  fixtures.
- **Create `tests/fixtures/sys-path/`** — minimal captured sysfs subtrees (the
  `subsystem` symlinks, kernel dir names, and the handful of sysattr files each
  handler reads: `port_no`, `devN.M`, `nsid`), one device per handler plus the
  nested composite `pci-usb-scsi` case.
- **Modify `Makefile`** — add `test_path_id` to the test build/run and to the
  clean target.
- **`schema-udev.c` — untouched.** path_id is not wired into the live daemon.
  Off by default, mechanism only, same boundary as Phase 3a. A hard grep/diff
  gate proves `schema-udev.c` is byte-identical to master.

## Error handling

- Unreadable `subsystem`, missing sysattr, or malformed sysname → the handler
  declines; if the walk ends with no pci/platform anchor, `path_id_build`
  returns −1 (no `ID_PATH`), matching udev.
- Output buffer too small → return −1. No partial/truncated paths.
- Never write to sysfs, never emit netlink, never touch `/run/udev`. Read-only,
  same safety class as the coldplug walker.

## Testing

**Unit (deterministic, CI, no hardware):** `tests/test_path_id.c` drives
`path_id_build` against `tests/fixtures/sys-path/` trees and asserts the exact
ID_PATH + ID_PATH_TAG for:
1. bare pci (`pci-0000:07:00.0`)
2. usb interface (`pci-0000:02:00.0-usb-0:4:1.0`)
3. ata disk (`pci-0000:02:00.1-ata-1.0`) — exercises `port_no` sysattr
4. nvme (`pci-0000:01:00.0-nvme-1`) — exercises `nsid` sysattr
5. scsi default with host rebase (`…-scsi-0:0:0:0`) — exercises H rebasing
6. platform (`platform-AMDI0030:00`)
7. nested composite (`pci-0000:08:00.3-usb-0:1:1.0-scsi-0:0:0:0`)
8. `path_id_tag` transform (`pci-0000:00:00.0` → `pci-0000_00_00_0`)
9. unanchored ancestry → −1

**Acceptance (live, Claire post-Greg):** run `path_id_build` over every device
in live `/sys` that real udev assigned an `ID_PATH`, diff each result against
the ground-truth value from `udevadm info` / `/run/udev/data`. **Require 0
mismatches across all 182 devices.** This is the same acceptance bar the parity
harness established.

**Boundary gate:** `git diff master..HEAD -- schema-udev.c` empty; `grep
path_id schema-udev.c` empty.

## Out of scope

- Wiring path_id into the live daemon (that is the deferred Phase 3b cutover:
  needs all builtins + the uaccess ACL manager + group-2 socket + real
  `/run/udev/data` writes, behind SSH recovery and a spare-boot fallback).
- Non-blakbox bus handlers (mmc, virtio, ccw, sas, …) — added later per-host as
  parity data is captured.
- `ID_PATH` rule-gating (which device classes udev runs path_id on) — a cutover
  concern, not the builtin's.
