# schema-udev builtin wiring (endgame sub-project A) — Design

**Status:** approved 2026-08-06
**Sub-project:** A of 5 in the udevd-retirement endgame (A: builtin wiring, B: db + group-2 rebroadcast, C: persistent symlinks, D: uaccess ACL manager, E: cutover). Built alongside udevd; verified by parity; udevd retired last (E).
**Boundary shift:** this is the sub-project where `schema-udev.c` / `schema-udev.h` **stop** being byte-identical. Across all 6 builtins the boundary held; wiring is exactly the act of lifting it. The change is minimized to a single call site plus one cap bump.

## Goal

schema-udev runs the six reimplemented builtins (path_id, usb_id, input_id, net_id, blkid, hwdb)
against each device — on both the coldplug sysfs walk and live hotplug uevents — with
udev-faithful dispatch guards, merging each builtin's output into the event's property set. This is
the `IMPORT{builtin}` equivalent. Acceptance is **0 mismatches vs real udev across all `/sys`
devices on blakbox** (both directions) on the union of the six builtins' owned property subsets.

**Compute-only / inert:** A attaches builtin properties and exposes them (hooks already receive the
full property set via environment in `run_hook`; rules can now `match_<KEY>` on builtin properties).
Symlink and hook *logic* is unchanged — nothing new fires. A is behaviorally inert on the live box;
its only observable is the parity gate. The consumers are B (db), C (by-uuid symlinks), and D.

## Prerequisite

All six builtins must be on `master`. hwdb (#87) merges before this sub-project's implementation
begins. Speccing/planning does not require the merge.

## Architecturally

Each builtin is already a proven, self-contained mechanism verified against live udev in isolation.
A adds the **orchestration layer**: which builtins run against which devices, and how their outputs
merge. The single hard part is reproducing udevd's dispatch guards faithfully enough that the
*aggregate* property set matches — udevd's builtin invocations are spread across ~30 shipped rule
files with per-line conditions, not a clean subsystem→builtin table. Getting a guard wrong shows up
as over-emission (schema emits a property udev's rules suppressed, e.g. `ID_PATH` on a virtual/loop
disk) or under-emission. The live parity gate is the authority on guard correctness.

## Normative reference & validation

Dispatch guards distilled from blakbox's installed `/usr/lib/udev/rules.d/*.rules`
(`IMPORT{builtin}` lines and their conditions) and systemd v259 `udev-rules.c` semantics. The
per-builtin emission is unchanged — A does not modify any builtin header's output. Source governs;
the live gate is the authority.

## Entry point

New orchestrator header `udev_builtins.h` includes all six builtin headers and exposes:

```c
int run_builtins(const char *sysroot, const char *devpath,
                 const char *devnode, struct uevent *ev);
```

- `sysroot` — `/sys` (or a test root).
- `devpath` — the device's `DEVPATH` (from the uevent), e.g. `/devices/pci0000:00/...`.
- `devnode` — the `/dev` node (from `DEVNAME`), or NULL if none; only blkid needs it.
- `ev` — the event, pre-populated with the kernel payload properties. `run_builtins` **appends** the
  builtins' properties in place. Returns the number of properties added (>= 0), or -1 on a hard
  error (does not partially corrupt `ev`).

`schema-udev.c` calls `run_builtins` in exactly one place: inside `dispatch()`, after the event is
parsed (hotplug) or synthesized (coldplug) and **before** the rule-matching loop, so
`match_ID_FS_TYPE=…`, `match_ID_NET_NAME_PATH=…`, etc. resolve against builtin properties. This is
the only functional change to `schema-udev.c`.

## Guarded dispatch

Run in this fixed order (matches udev rule precedence). Each builtin appends into `ev`; on a
key collision the later writer wins (matches udev's later-`IMPORT` behavior). Builtins already
return nothing when inapplicable — the guards additionally prevent over-emission (properties udev's
rules would have suppressed) and wasteful raw I/O (blkid).

| order | builtin | guard | owned keys |
|---|---|---|---|
| 1 | hwdb | `modalias` sysattr present (`MODALIAS!=""`) | `ID_VENDOR_FROM_DATABASE`, `ID_MODEL_FROM_DATABASE`, `ID_PCI_CLASS_FROM_DATABASE`, `ID_PCI_SUBCLASS_FROM_DATABASE`, and other single-key `*_FROM_DATABASE` (composite input/net:naming/OUI lookups deferred, excluded from gate) |
| 2 | path_id | `SUBSYSTEM∈{pci,usb,platform}`, OR block `DEVTYPE=disk` and `DEVPATH` not matching `*/virtual/*` (incl. nvme-subsystem), OR an ancestor `SUBSYSTEMS∈{pci,usb,platform,acpi}` | `ID_PATH`, `ID_PATH_TAG` |
| 3 | usb_id | `SUBSYSTEM=usb` and `DEVTYPE=usb_device` | `ID_VENDOR`, `ID_VENDOR_ID`, `ID_MODEL`, `ID_MODEL_ID`, `ID_SERIAL`, `ID_SERIAL_SHORT`, `ID_REVISION`, `ID_TYPE`, `ID_USB_*`, `ID_BUS`, `ID_INSTANCE` (per usb_id.h) |
| 4 | input_id | `SUBSYSTEM=input` | `ID_INPUT`, `ID_INPUT_*` |
| 5 | net_id | `SUBSYSTEM=net` | `ID_NET_NAMING_SCHEME`, `ID_NET_NAME_MAC`, `ID_NET_NAME_ONBOARD`, `ID_NET_LABEL_ONBOARD`, `ID_NET_NAME_PATH`, `ID_NET_NAME_SLOT` |
| 6 | blkid | `SUBSYSTEM=block`, `KERNEL` not matching `sr*` or `mmcblk*boot*`, `DEVTYPE∈{disk,partition}` | `ID_PART_TABLE_*`, `ID_PART_ENTRY_*` (blkid_pt), `ID_FS_*` identity (blkid_fs) |

Guard inputs come from the event itself where present (`SUBSYSTEM`, `DEVTYPE`, `DEVPATH`, `DEVNAME`)
and from sysfs otherwise (`modalias` via `pi_sysattr`; ancestor subsystem via walking `DEVPATH`
parents and reading each `subsystem` symlink basename). `KERNEL` is the sysfs kobject name (basename
of `DEVPATH`).

## Merge semantics

1. `ev` enters with the kernel payload properties already parsed in.
2. Each builtin, in dispatch order, appends its `key=value` pairs.
3. On a key already present in `ev`: overwrite the value (later writer wins). On our fleet no builtin
   key collides with a base kernel key; collisions between builtins are resolved by dispatch order.
4. Key cap: `UE_MAX_KEYS` raised 32 → 64. Worst observed aggregate is a block partition (~17) and a
   USB device (~18); 64 gives ample headroom so a device can never silently truncate. `UE_KEY_MAX`
   and `UE_VAL_MAX` unchanged.

`ub_merge` (or the append path in `run_builtins`) enforces the overwrite rule and the cap; on cap
overflow it drops the excess and the caller logs (a dropped property would fail the parity gate, so
the cap is sized to never trigger on real devices).

## Components

- **`udev_builtins.h`** (new): includes `path_id.h`, `usb_id.h`, `input_id.h`, `net_id.h`,
  `blkid_fs.h` (which pulls `blkid_pt.h`), `hwdb.h`. Public `run_builtins(...)`. Internals:
  `ub_has_modalias(sysroot, devpath)`, `ub_ancestor_in(sysroot, devpath, const char *const *subs)`
  (walk parents, match any `subsystem` basename), `ub_kernel_name(devpath, out, sz)`,
  `ub_fnmatch_kernel(pattern, name)`, and `ub_append(ev, key, val)` (overwrite-or-add with cap).
  Guard predicates read `SUBSYSTEM`/`DEVTYPE`/`DEVPATH` from `ev` via `uevent_get`.
- **`tests/test_udev_builtins.c`** (new): synthetic `/sys` trees + fabricated events in a tmpdir.
  Assert the correct builtin *set* fires per guard and merge is correct:
  - usb_device event → usb_id + hwdb keys present, blkid absent.
  - virtual/loop disk (`DEVPATH` contains `/virtual/`) → `ID_PATH` absent (path_id suppressed),
    blkid still runs.
  - `sr0` (`KERNEL=sr0`) → blkid suppressed.
  - net device → net_id keys present; input device → input_id keys present.
  - merge dedup: a fabricated collision resolves to the later-dispatch builtin's value.
  - key-cap headroom: a device producing ~18 keys keeps all of them (none dropped).
  Reuse the fabricated-superblock / synthetic-sysfs helpers from the existing per-builtin tests.
- **`tests/verify_builtins_live.sh`** (new): for every device under `/sys` (enumerate as the
  per-builtin live gates do), build the event's kernel payload, run `run_builtins`, and diff the
  emitted property set against `udevadm info -q property <dev>` — restricted to the **union of the
  six builtins' owned key-subsets**, both directions. `sudo` (blkid raw reads). Excludes the same
  deferred keys the per-builtin gates excluded: `ID_OUI_FROM_DATABASE`, `ID_NET_DRIVER`,
  `ID_FS_SIZE`, `ID_FS_BLOCKSIZE`, `ID_FS_LASTBLOCK`. Expect **0 mismatches** across all devices.
- **`schema-udev.c`**: one `run_builtins` call site in `dispatch()` before the match loop. No other
  logic change.
- **`schema-udev.h`**: `#define UE_MAX_KEYS 64` (was 32). No other change.
- **`Makefile`**: one line to build+run `tests/test_udev_builtins.c`.

## Testing / acceptance gate

1. `make test` green incl. `test_udev_builtins`, `-Wall -Wextra` clean.
2. Boundary: `git diff master -- schema-udev.c` shows only the single `run_builtins` call site;
   `git diff master -- schema-udev.h` shows only the `UE_MAX_KEYS` bump. No other lines.
3. Live: `tests/verify_builtins_live.sh` → **0 mismatches** across all `/sys` devices, both
   directions, on the union owned-subset.
4. `vmtest.sh` → RESULT: PASS (schema-udev is not PID 1; the vmtest rail must still pass unchanged).

## Out of scope

- The `/run/udev/data/` database writer and group-2 rebroadcast (sub-project B).
- Persistent `/dev/disk/by-*`, `/dev/input/by-*` symlinks and symlink var-expansion (sub-project C).
- The uaccess ACL manager (sub-project D).
- Retiring `udevd.svc` — the cutover (sub-project E).
- Composite hwdb lookups (evdev/mouse/keyboard/sensor/net:naming/OUI) — deferred with the rules
  engine's multi-attribute key construction.
- Any consumption of the new properties beyond hooks-via-environment and rule `match_` conditions.
- A declarative `builtin=` rule field (auto-by-subsystem dispatch is the parity baseline; the
  declarative form is YAGNI until a concrete need appears).

## Notable risk

Guard fidelity is the whole game. The per-builtin gates proved each builtin emits udev-exact output
*when invoked*; A's new risk is invoking the wrong builtin on the wrong device (over/under-emission).
The `*/virtual/*` path_id guard, the `sr*|mmcblk*boot*` blkid guard, the usb `DEVTYPE=usb_device`
guard, and the `MODALIAS!=""` hwdb guard are the ones most likely to bite — each maps to a real
device class on blakbox (loop/zram, the optical/mmc nodes, usb interfaces vs devices, buses without
a modalias). The aggregate live gate over every `/sys` device, both directions, is the final
authority — the same loop that caught four spec bugs across the six builtins.
