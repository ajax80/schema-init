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

| Tree (`/dev/schema/disk/…`) | Link name | Gate |
|---|---|---|
| `by-uuid/<name>`     | `ID_FS_UUID_ENC` (fallback `ID_FS_UUID`) | property present |
| `by-label/<name>`    | `ID_FS_LABEL_ENC`                        | property present |
| `by-partuuid/<name>` | `ID_PART_ENTRY_UUID`                     | property present |
| `by-partlabel/<name>`| `ID_PART_ENTRY_NAME` (already ENC)       | property present |
| `by-path/<name>`     | `ID_PATH` (+ `-part<PARTN>` if partition)  | property present |
| `by-diskseq/<name>`  | `DISKSEQ` (+ `-part<PARTN>` if partition)  | property present |

**Partition suffix rule (critical for parity):** udevd stores a single
`ID_PATH`/`DISKSEQ` value shared by a disk and its partitions, then appends
`-part<PARTN>` to the *link name* for partitions (verified on hardware:
`sda` → `by-diskseq/2`, `sda1` → `by-diskseq/2-part1`; `sda` →
`by-path/pci-…-ata-1.0`, `sda1` → `by-path/pci-…-ata-1.0-part1`). So
`by-path` and `by-diskseq` apply to BOTH disks and partitions; the partition
link name is `<value>-part<PARTN>`. `PARTN` is a kernel uevent property
(present on add AND remove, absent from the shadow db). The four other trees
(`by-uuid`/`by-label`/`by-partuuid`/`by-partlabel`) are never suffixed.

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
int disk_links_derive(const struct uevent *ev, struct disk_link *out, int max);
```
The shared core: for a given uevent, produce the list of `(tree, name)`
pairs the device should own — used by BOTH apply and gc so the two can never
disagree. Applies the partition suffix rule (`-part<PARTN>` for `by-path`
and `by-diskseq` when `DEVTYPE=partition`). Returns the count.

```
int disk_links_apply(const char *base_dir, const struct uevent *ev);
```
Derive the pairs from the live uevent, then for each create
`<base_dir>/<tree>/<value>` as a symlink to a **relative** target that
resolves back to the devnode — `../../../<devname>` (three `..` climb:
`by-X` → `disk` → `schema` → `dev`), matching udev's relative-symlink
convention. `mkdir -p` the tree directory; write atomically via a
`.tmp.<pid>` name + `rename()` (same technique as `symlink_apply` in
`schema-udev.h`). Best-effort per link: a failure on one tree does not abort
the others. Returns 0. (`base_dir` is `SCHEMA_DISK_DIR` in production; a
temp dir in tests.)

```
int disk_links_gc(const char *base_dir, const char *db_dir, const struct uevent *ev);
```
On remove, the shadow db record carries the DERIVED props
(`ID_FS_*`/`ID_PART_*`/`ID_PATH`) but NOT the kernel props, while the live
remove uevent carries the kernel props (`DEVTYPE`/`DISKSEQ`/`PARTN`) but NOT
the derived ones (device gone, no re-probe). So gc **merges** both: read the
db record at `<db_dir>/b<maj>:<min>` via the existing
`udev_db_read_eprops` (giving the derived props), then graft `DEVTYPE`,
`DISKSEQ`, and `PARTN` from the live `ev`. Run `disk_links_derive` on the
merged uevent and `unlink()` each `<base_dir>/<tree>/<name>` (ignoring
`ENOENT`). Returns 0.

```
void disk_links_wipe(const char *base_dir);
```
Recursively remove `base_dir` (symlinks + empty dirs) via
`nftw(..., FTW_DEPTH | FTW_PHYS)`. Called once at daemon startup on
`SCHEMA_DISK_DIR` before coldplug, so the farm is rebuilt clean each boot
(this is the GC for devices that vanished while the daemon was down).
`FTW_PHYS` ensures our entries are unlinked, not followed.

### Wiring in `schema-udev.c`

In `dispatch()`, gate on `SUBSYSTEM=block`, full stop —
`uevent_get(ev, "SUBSYSTEM")` is reliably set by `uevent_from_sysfs` via
subsystem-symlink resolution. Do NOT gate on `MAJOR` (fragile: SCSI disk
and optical share major ranges). Optical devices (`sr0`) ARE
`SUBSYSTEM=block` and DO get `by-label`/`by-uuid` links from this path —
that is correct and intended (their FS properties flow in via
`cdrom_id.h`/`optical_fs.h`). Mirrors the existing `symlink=` rule block:

- `add` / `change`: after `run_builtins` + `run_rules` populate `ev` and
  after `udev_db_write`, call `disk_links_apply(SCHEMA_DISK_DIR, ev)`.
- `remove`: call
  `disk_links_gc(SCHEMA_DISK_DIR, SCHEMA_UDEV_DB_DIR, ev)` **before**
  `udev_db_remove(...)` — the db record must still exist when gc reads it.

The `b<maj>:<min>` key is built inside gc from the uevent via
`udev_db_filename` (block → `b<maj>:<min>`), the same helper `udev_db.h`
uses.

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

remove:        uevent → disk_links_gc(ev)   (merge db record + kernel props)
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
- `change` event that alters a source property (e.g. relabel, or an optical
  disc swap on `sr0`: `POWERT_TOUR_DVD` ejected → blank inserted) re-applies
  the new link but does NOT prune the now-stale old-value link until the
  next `remove` or the startup wipe. Accepted for slice C (udevd's full
  db-diff pruning is deferred). Parity is measured on a fresh coldplugged
  daemon, so this does not affect the gate.
  **Follow-up (slice D/E):** add a db-diff prune on `change` — read the
  still-present OLD db record's derived link names *before* `udev_db_write`
  overwrites it, then `unlink` any no longer produced by the new `ev`. The
  shared derive helper + existing `disk_links_gc` make this nearly free;
  held out of slice C only to keep the surface tight.

## Testing

**Unit — `tests/test_disk_links.c`:**
- Synthesize a `struct uevent` for a whole disk (`DEVTYPE=disk`,
  `DEVNAME=sda`, `MAJOR`/`MINOR`, `DISKSEQ`, `ID_FS_UUID_ENC`,
  `ID_PATH`) into a temp root; call `disk_links_apply`; assert the expected
  links exist with exact names and each relative target reads back as
  `../../../sda`.
- Synthesize a partition uevent (`DEVTYPE=partition`, `DEVNAME=sda1`,
  `PARTN=1`, `DISKSEQ`, `ID_PATH`, `ID_PART_ENTRY_UUID`,
  `ID_PART_ENTRY_NAME`); assert `by-path` and `by-diskseq` link names carry
  the `-part1` suffix (e.g. `by-diskseq/2-part1`,
  `by-path/pci-…-ata-1.0-part1`) while `by-partuuid`/`by-partlabel` do NOT.
- Assert `by-partlabel` preserves the pre-encoded value verbatim (e.g.
  `Basic\x20data\x20partition`).
- Write a matching shadow-db record (derived props only) and call
  `disk_links_gc` with a live remove-ev carrying only the kernel props
  (`DEVTYPE`/`DISKSEQ`/`PARTN`/`MAJOR`/`MINOR`); assert the merge recovers
  and unlinks ALL derived links including the suffixed `by-diskseq`/`by-path`
  — proving the db+ev merge.
- Assert `disk_links_wipe` on a populated temp farm removes the whole
  subtree.
- Must build and pass under the existing `make test` harness.

**Live parity gate — `tests/verify_disk_links_live.sh` (sudo):**
- Spawn a fresh daemon (`rm -rf /run/schema-udev`, start `./schema-udev`),
  let coldplug settle.
- For each of the six in-scope trees: compare **symlinks only**
  (`find -type l -printf '%f\n'`, so stray non-symlink entries like udevd's
  anomalous `…-part` directory are ignored) — assert the link-name set under
  `/dev/schema/disk/by-X` **equals** the set under `/dev/disk/by-X`, and that
  each shadow link `realpath`s to the same device node as udevd's
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
