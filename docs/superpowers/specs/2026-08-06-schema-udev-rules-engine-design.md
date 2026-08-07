# schema-udev rules engine (endgame sub-project B, slice 1: property completeness) — design

## Context

The udevd-retirement endgame decomposes into five sub-projects, each built
alongside a still-running udevd and verified by parity before udevd is retired
last: **A** builtin wiring (`IMPORT{builtin}`, landed as PR #88), **B** the
rules engine, **C** persistent `/dev/disk/by-*` symlinks, **D** uaccess ACL
manager, **E** the cutover.

Sub-project A wired the six reimplemented builtins so `run_builtins()` computes,
per device, the properties that builtin is the *origin* for. A was accepted with
an **origin-scoped** parity gate: each builtin compared only on its own origin
device class. Two udev property mechanisms were explicitly deferred to B because
they are not `IMPORT{builtin}`:

1. **`IMPORT{parent}` propagation** — a child device inherits `ID_*` properties
   from an ancestor (proven in A: `hidraw5`'s `ID_USB_VENDOR` is byte-identical
   to its parent `usb_device`; ACPI vendor is inherited, not freshly computed).
2. **Constructed/composite hwdb keys** — udev builds a synthetic modalias for a
   device class (`usb:vVVVVpPPPP…` from `idVendor`/`idProduct`, `OUI:…` from a
   MAC, `acpi:…`, `pci:…`), queries hwdb with it, and attaches the resulting
   `*_FROM_DATABASE` keys. A's hwdb builtin only handled the device's *own*
   non-empty `modalias` sysattr.

These two mechanisms are exactly what stands between A's origin-scoped parity and
**full per-device parity** with real udevd. Slice 1 of B implements them.

B has three further slices, out of scope here: the live `/run/udev/data` db
writer, the group-2 libudev rebroadcast, and reimplementing the remaining
builtins (`ata_id`/`scsi_id`/`cdrom_id`/`v4l_id`/`mtd_probe`). This spec is
property completeness only, and it is compute-only/inert exactly like A.

## Goal

After `run_builtins()` runs per device, add the two remaining property-derivation
mechanisms so schema-udev's full per-device property set matches real udevd's
`/run/udev/data` `E:` lines — for **all** devices (children included, not only
origin devices), with 0 missing and 0 mismatched values, scoped to the
properties owned by the six reimplemented builtins and their
inheritance/composites.

## Existing groundwork (already in tree, unit-tested, unwired)

Slice 1 builds on primitives that already exist and are tested:

- `udev-parity.h` / `tools/udev-parity.c` — read-only harness that coldplug-walks
  `/sys` and diffs schema-udev's synthesized properties against real
  `/run/udev/data`. **Bug it has today:** `collect()` diffs the *raw kernel*
  uevent props (it never runs the builtins), so it reports almost everything as
  missing. Slice 1 fixes this — see "Component 3".
- `parity_builtin_hint(key)` (in `udev-parity.h`) — classifies an `E:` key by its
  owning builtin (`hwdb`, `input_id`, `net_id`, `blkid`, `path_id`, `v4l_id`,
  `usb_id`), or `""` if unattributable. This is the mechanism that makes the
  honest gate scoping mechanical rather than a fudge.
- `hwdb_build()` / `hwdb.h` — hwdb trie lookup (A). Reused for composite lookups.
- `pi_parent()` / `pi_subsystem()` / `pi_sysattr()` (in `path_id.h`, used
  throughout `udev_builtins.h`) — ancestor walk and sysattr reads.
- `ub_add()` / `ub_absorb()` (in `udev_builtins.h`) — first-writer-wins merge.

## Architecture

### Component 1: `udev_rules.h` (new) — the post-pass

A single dedicated header (the "rules engine"; A's `udev_builtins.h` stays purely
mechanism). It exposes one entry point:

```c
/* Post-builtin property derivation. Runs after run_builtins() has populated ev
 * with this device's own builtin properties. Additive + first-writer-wins:
 * never overwrites the kernel payload or a builtin's own output. Returns the
 * number of keys added. */
static inline int run_rules(const char *sysroot, const char *devpath,
                            const char *devnode, struct uevent *ev);
```

`run_rules()` performs two passes, in this order:

**Pass 1 — `IMPORT{parent}` inheritance.** Walk the ancestor chain nearest-first
(`pi_parent`). For each ancestor, compute its properties with `run_builtins()`
into a scratch `struct uevent`, then inherit into `ev` (via `ub_add`, so
first-writer-wins) only the keys in an **inheritable-key set**. The child's own
values always win because `run_builtins()` already populated `ev` before
`run_rules()` runs.

The inheritable-key set is bounded empirically by the measured parity gap
(Component 3), not by inheriting all `ID_*` blindly. udev does not propagate
every `ID_*` from every ancestor; the gap measurement tells us exactly which
keys udev inherits in practice on this hardware, and the set is restricted to
those. Expected members (to be confirmed by measurement): `ID_PATH`,
`ID_PATH_TAG`, the `ID_USB_*` family, and the `*_FROM_DATABASE` family.

**Pass 2 — composite hwdb.** For the device's class, construct the synthetic
modalias udev builds, `hwdb_query` it (reusing `hwdb.h`), and merge the resulting
`*_FROM_DATABASE` keys (via `ub_add`). Only the classes the measured gap actually
exercises are implemented — no speculative classes. Expected (to be confirmed by
measurement): usb (`usb:vVVVVpPPPP…` from `idVendor`/`idProduct`/`bcdDevice`
sysattrs), OUI (`OUI:XXXXXX` from the first three MAC octets), acpi, pci.

### Component 2: live wiring (one line)

`schema-udev.c` `dispatch()` gains a single call immediately after the existing
`run_builtins(...)` call site:

```c
run_builtins("/sys", devpath, dn, ev);
run_rules("/sys", devpath, dn, ev);   /* B: IMPORT{parent} + composite hwdb */
```

`schema-udev.h` is untouched. `udev_rules.h` includes `udev_builtins.h` (for
`run_builtins`, `ub_add`, `ub_absorb`) and `hwdb.h`.

### Component 3: fix `tools/udev-parity.c`

`collect(ev)` currently diffs the raw coldplug uevent against the db. Change it to
run `run_builtins()` then `run_rules()` on a mutable copy of `ev` before diffing,
so the harness measures the *actual* post-A+B gap. This is what makes the tool
usable both as the measurement instrument (to derive the inheritable-key set and
composite classes) and as the acceptance gate's engine.

## Data flow

```
kernel/coldplug uevent
  → run_builtins()   (A: own-device IMPORT{builtin})
  → run_rules()      (B: IMPORT{parent} inheritance + composite hwdb)
  → rule match / hooks / db-parity
```

**Inertness contract (unchanged from A):** B computes and attaches properties
only. No new symlink is created, no hook fires, nothing is broadcast. Rules may
`match_` the new properties and hooks receive them via env — identical to A.

## Merge semantics

All additions go through `ub_add()` (first-writer-wins): a key already present —
whether from the kernel payload, a builtin, an earlier ancestor, or the composite
pass — is never overwritten. Ancestor walk is nearest-first so the closest
ancestor's value wins among ancestors. This is the same discipline A established;
B introduces no new merge rule.

## Error handling

Consistent with the codebase: no device is fatal. A missing sysattr, an ancestor
with no properties, an absent modalias component, or a hwdb miss simply
contributes nothing. `run_rules()` never fails the dispatch; it returns the count
added (0 is valid).

## Testing

### Unit — `tests/test_udev_rules.c`

Synthetic sysfs trees (`mkdtemp` + mkdirs + subsystem symlinks, same pattern as
`test_udev_builtins.c`):

- **inherit:** a child whose parent carries `ID_USB_VENDOR` (seeded via the
  parent's computable props) inherits it when the child lacks it.
- **first-writer-wins:** a child that already has its own `ID_PATH` keeps its own
  value; the ancestor's is not applied.
- **non-inheritable key not propagated:** a key outside the inheritable set on an
  ancestor is not copied to the child.
- **composite usb modalias:** `idVendor=1d6b`, `idProduct=0002`, `bcdDevice=…`
  sysattrs construct the exact `usb:vVVVVpPPPP…` string udev builds.
- **OUI:** a MAC sysattr constructs the correct `OUI:XXXXXX` lookup key.

### Live gate — `tests/verify_rules_live.sh`

Full-device parity via the fixed `tools/udev-parity` (built with `run_builtins` +
`run_rules`). For every device with a real `/run/udev/data` entry, every `E:` key
**owned by the six reimplemented builtins and their inheritance/composites** must
be reproduced with the identical value. Result must be **0 missing and 0
mismatched**, across all `/sys` devices (not origin-scoped — this is the A→B
upgrade: children too). `sudo` (blkid reads raw block devices).

**Honest scope of the gate — what is excluded and why (documented, not hidden):**

- Keys owned by builtins **not yet reimplemented**: `ata_id`/`scsi_id`/`cdrom_id`
  (`ID_ATA_*`, and `ID_SERIAL`/`ID_MODEL`/`ID_VENDOR` on non-usb block/optical
  devices), `v4l_id` (`ID_V4L_*`/`ID_VIDEO_*`), `mtd_probe`. These are later-slice
  work; comparing them now would fail for reasons B does not own.
- Pure-runtime/db-management keys udevd stores that are not builtin/rule-derived:
  `USEC_INITIALIZED`, tags, seat/current-tags bookkeeping.
- `ID_SERIAL`/`ID_MODEL`/`ID_VENDOR` are trusted **only on the usb device chain**
  (they also originate from ata/scsi/cdrom elsewhere) — the same origin
  discipline A applied to `usb_id`, now extended down the inheritance chain.

The exclusion is mechanical: `parity_builtin_hint()` attributes each `E:` key to
an owning builtin, and the gate restricts the comparison to keys attributed to
the six we reimplemented. This is the same anti-hollow-gate discipline enforced in
A: the gate must compare every in-scope device and key, and a shrinking device
count is the smell that catches a hollow gate.

### `make test`

`test_udev_rules` added to the suite; full suite green, `-Wall -Wextra` clean.

## Acceptance criteria

1. `tools/udev-parity` runs `run_builtins` + `run_rules` and reports **0 missing,
   0 mismatched** in-scope keys across all `/sys` devices with a udev db entry.
2. `tests/verify_rules_live.sh` exits 0 with a non-trivial device/key count (the
   count must reflect all in-scope devices, not a narrowed subset).
3. `tests/test_udev_rules.c` passes; `make test` green; `-Wall -Wextra` clean.
4. Boundary minimal: `schema-udev.c` +1 call site, `schema-udev.h` untouched.
5. vmtest still PASS (schema-udev is not PID 1; the rail is unchanged).

## Out of scope (later B slices, and beyond)

- Live `/run/udev/data` db writer (slice 2) — `udev_db_write()` exists and is
  unit-tested but stays unwired; when wired it targets a shadow dir alongside
  udevd, never udevd's real dir.
- Group-2 libudev rebroadcast (slice 3) — genuinely hazardous alongside a running
  udevd (double-broadcast); belongs next to the cutover (E).
- Reimplementing `ata_id`/`scsi_id`/`cdrom_id`/`v4l_id`/`mtd_probe`.
- Persistent `/dev/disk/by-*` symlinks (sub-project C).
