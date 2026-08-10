# schema-udev slice C — persistent /dev/disk/by-* symlink farm (shadow tree)

**Date:** 2026-08-09
**Sub-project:** schema-udev endgame, slice C (udevd retirement, read-side).
**Status:** design approved.

## Goal

Turn the ID_* properties schema-udev already emits into the actual
`/dev/disk/by-*` symlink trees that udevd currently owns — but built under a
**shadow tree** (`/dev/schema/disk/by-*`) so it never fights live udevd over
`/dev/disk`. Six mechanical trees this slice; `by-id` is deferred to its own
slice.

## Scope

**In scope — six trees, each one-property-one-link:**

| Tree (`/dev/schema/disk/…`) | Link-name source property | Gate |
|---|---|---|
| `by-uuid/<name>`     | `ID_FS_UUID_ENC` (fallback `ID_FS_UUID`) | property present |
| `by-label/<name>`    | `ID_FS_LABEL_ENC`                        | property present |
| `by-partuuid/<name>` | `ID_PART_ENTRY_UUID`                     | property present |
| `by-partlabel/<name>`| `ID_PART_ENTRY_NAME` (already ENC)       | property present |
| `by-path/<name>`     | `ID_PATH`                                | property present |
| `by-diskseq/<name>`  | `DISKSEQ`                                | `DEVTYPE=disk` |

All six source properties are ALREADY emitted by existing builtins
(`blkid_fs.h` emits `ID_FS_UUID`/`ID_FS_UUID_ENC`/`ID_FS_LABEL_ENC`;
`blkid_pt.h` emits `ID_PART_ENTRY_UUID`/`ID_PART_ENTRY_NAME` pre-encoded;
`udev_builtins.h` emits `ID_PATH`; `DISKSEQ` rides in the kernel uevent).
**Slice C adds no probing and no new string encoder** — it is pure
property→symlink plumbing.

**Out of scope (deferred):**
- `by-id` — its multi-link, bus-specific parity (`ata-`/`nvme-` [three name
  variants]/`wwn-`/`usb-`/`scsi-` prefixes, model+serial sanitization,
  `-partN` suffixes) is a separate slice.
- Writing to the real `/dev/disk` — that cutover is a one-constant flip
  deferred to **slice E**, when udevd is actually retired.
- `link_priority`-style collision arbitration — last-writer-wins (udev's
  default without an explicit priority) is sufficient.

## Coexistence strategy (the central decision)

**Shadow tree at `/dev/schema/disk/by-*`.** schema-udev never touches
`/dev/disk` while udevd is live. This is the same discipline already applied
to the shadow db (`/run/schema-udev/data` vs udevd's `/run/udev/data`) and
the Phase-2 `symlink=` tree (`/dev/schema`). Zero path contention with
udevd. Parity is verified by comparing the two farms; the destructive
cutover to `/dev/disk` belongs to slice E.

Rejected alternatives: writing directly to `/dev/disk/by-*` (both daemons
race; GC is fatal — schema-udev would unlink paths udevd still owns);
direct-write-without-GC (leaks stale links, still contends on create).

## Architecture

**New file: `disk_links.h`** — one-header-per-feature, self-contained,
included by `schema-udev.c` (mirrors `optical_fs.h`).

### Constants
- `SCHEMA_DISK_DIR "/dev/schema/disk"` — root of the shadow farm.

### Public functions

```
int disk_links_apply(const struct uevent *ev);
```
For each table row whose source property is set (and gate passes), create
`/dev/schema/disk/<tree>/<value>` as a symlink to a **relative** target that
resolves back to the devnode — `../../../<devname>` (three `..` climb:
`by-X` → `disk` → `schema` → `dev`), matching udev's relative-symlink
convention. `mkdir -p` the tree directory; write atomically via a
`.tmp.<pid>` name + `rename()` (same technique as the existing
`symlink_apply` in `schema-udev.h`). Best-effort per link: a failure on one
tree does not abort the others. Returns 0 (best-effort; individual failures
are logged to stderr, not fatal).

```
int disk_links_gc(const char *maj_min);
```
Read the shadow db record at `/run/schema-udev/data/<maj_min>` (e.g.
`b8:1`), parse its `E:KEY=VALUE` lines into a temporary `struct uevent`, run
the SAME derive logic as `disk_links_apply`, and `unlink()` each resulting
path (ignoring `ENOENT`). This recovers the link names for a device whose
kernel remove-uevent is too sparse to re-derive them. Returns 0.

A shared static helper derives, for a given `struct uevent`, the list of
`(tree, name)` pairs — used by both apply (to create) and gc (to unlink), so
the two paths can never disagree.

### Wiring in `schema-udev.c`

In `dispatch()`, block devices only (guard on `MAJOR`/`SUBSYSTEM=block` or
presence of a block devnode — match the existing convention), mirroring the
existing `symlink=` rule block:

- `add` / `change`: after `run_builtins` + `run_rules` populate `ev` and
  after `udev_db_write`, call `disk_links_apply(ev)`.
- `remove`: call `disk_links_gc("<maj>:<min>")` **before**
  `udev_db_remove(...)` — the db record must still exist when GC reads it.

The `<maj>:<min>` key is built from the uevent `MAJOR`/`MINOR` the same way
`udev_db.h` builds its record path (block → `b<maj>:<min>`).

### Startup wipe

In `main()` init (before coldplug), recursively remove `/dev/schema/disk`
only — the Phase-2 `symlink=` links live at `/dev/schema/<name>` and MUST
be left intact. Coldplug then repopulates a clean farm. This is the GC for
devices that vanished while the daemon was down, and guarantees the farm
reflects exactly the current device set on every start.

## Data flow

```
add/coldplug:  uevent → run_builtins (fills ID_* props) → udev_db_write
                                          ↓
                               disk_links_apply(ev)
                                          ↓
                       /dev/schema/disk/by-*/<value> → ../../../<devname>

remove:        uevent → disk_links_gc("b<maj>:<min>")   (reads db record)
                                          ↓                    ↓
                               unlink each derived path   → udev_db_remove
```

## Error handling

- Property absent → skip that tree silently (normal case).
- `mkdir`/`symlink`/`rename` failure on one tree → log to stderr, continue
  with the remaining trees (best-effort).
- `unlink` during GC → ignore `ENOENT`.
- Collision (two devices resolve to the same `by-label` name) →
  last-writer-wins (udev default; acceptable, no true collisions on the
  target hardware).
- `change` event that alters a source property (e.g. relabel) re-applies
  the new link but does NOT prune the now-stale old-value link until the
  next `remove` or the startup wipe. Accepted for slice C (rare; udevd's
  full db-diff pruning is deferred). Parity is measured on a fresh
  coldplugged daemon, so this does not affect the gate.

## Testing

**Unit — `tests/test_disk_links.c`:**
- Synthesize a `struct uevent` carrying all six source properties plus
  `DEVNAME`/`MAJOR`/`MINOR`/`DEVTYPE=disk`, pointed at a temp root; call
  `disk_links_apply`; assert all six link files exist with the exact
  expected names and that each relative target resolves to the devnode.
- Assert the `by-diskseq` gate: a `DEVTYPE=partition` uevent produces no
  `by-diskseq` link.
- Assert `by-partlabel` preserves the pre-encoded value verbatim (e.g.
  `Basic\x20data\x20partition`).
- Write a matching shadow-db record, call `disk_links_gc`, assert all six
  links are gone.
- Must build and pass under the existing `make test` harness.

**Live parity gate — `tests/verify_disk_links_live.sh` (sudo):**
- Spawn a fresh daemon (`rm -rf /run/schema-udev`, start `./schema-udev`),
  let coldplug settle.
- For each of the six in-scope trees: assert the link-name set under
  `/dev/schema/disk/by-X` **equals** the set under `/dev/disk/by-X`, and
  that each shadow link `realpath`s to the same device node as udevd's
  corresponding link.
- **`by-id` is explicitly excluded** from the comparison (deferred slice).
- Exit non-zero on any mismatch in either direction.

**vmtest:** `cd ~/schema-livetest && ./vmtest.sh` must stay green — slice C
touches only block-device symlink creation, no PID-1 path, so no regression
expected; the gate confirms it.

## Boundaries (what MUST NOT change)

- `schema-udev.c` netlink group stays `sa.nl_groups = 1` (group-1 only).
- No writes to `/dev/disk`, `/run/udev`, or any udevd-owned path.
- Phase-2 `symlink=` behavior and `/dev/schema/<name>` links unchanged.
- `run_builtins` / `ub_select` / the parity key classifier
  (`udev-parity.h`) unchanged — slice C adds a symlink-farm surface, not a
  property-key surface.

## Success criteria

1. `make test` green, including the new `test_disk_links` subtests.
2. Live parity gate: six in-scope trees match udevd set-wise and by resolved
   device, 0 mismatches both directions, `by-id` excluded.
3. Daemon shadow farm survives an add→remove cycle cleanly (no stale links).
4. vmtest PASS.
5. Zero changes to udevd-owned paths; boundaries above intact.
