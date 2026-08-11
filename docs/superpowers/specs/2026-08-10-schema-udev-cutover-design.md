# schema-udev Cutover (slice E) — Design Spec

> The flip: retire real `systemd-udevd`, make schema-udev the sole owner of
> post-pivot device management. Decomposed into three reboot-provable slices
> (E1 parity gate → E2 live-capable binary → E3 cutover). Tonight builds E1 + E2.

## Context / current state (as of 2026-08-10, master `3f417f7`)

schema-udev runs live as a supervised service (`state=FUNDAMENTAL`), listening on
the kernel uevent netlink **group 1**, **alongside** real `systemd-udevd`. It
reproduces everything udevd does but into **isolated namespaces** — it mutates
nothing real:

| capability      | dry-run target (today)      | real owner (still udevd) |
|-----------------|-----------------------------|--------------------------|
| dev symlinks    | `/dev/schema`               | `/dev`                   |
| disk links      | `/dev/schema/disk`          | `/dev/disk`              |
| udev db         | `/run/schema-udev/data`     | `/run/udev/data`         |
| uaccess (ACLs)  | `/run/schema-udev/uaccess` (decision files, **recorded not applied**) | real ACLs via `acl_set_file` |
| libudev monitor | *(nothing)*                 | group-2 broadcast        |

Proven invariant today: `strings /usr/bin/schema-udev | grep -c acl_set_file == 0`
— the deployed binary is physically incapable of mutating an ACL.

## Boot-criticality (the de-risker)

`/` resolves inside **initramfs** (its own udev instance, pre-pivot). By the time
schema-init is PID 1, root is already mounted. schema-udev therefore **cannot
brick the boot on root.** What it owns post-pivot:

- Real `/dev/disk/*` links → the `/mnt/*` mounts (all `nofail`) and `/home`
  (btrfs subvol on the same UUID as `/`, so unaffected).
- Real ACLs → audio/input/video usability (not boot survival).
- Group-2 libudev rebroadcast → GUI/mount helpers using libudev monitor.
- Real `/run/udev/data` → `udevadm info` / libudev queries.

None of these can fail the boot to an unusable state; the worst case is a
degraded desktop (no audio ACL, missing `/mnt/*`), which the parachute recovers.

## The hard constraint

Every one of the four capabilities is a **single-owner resource.** Two daemons
cannot both write real `/dev`, both apply ACLs, or both broadcast group 2 without
racing or double-firing. The cutover is therefore fundamentally atomic: udevd
stops owning them, schema-udev starts, gated by one boot-time flag, with a
next-boot fallback.

---

## The flag

File sentinel **`/etc/schema-init/schema-udev.live`**, read **once at startup**
into a global `g_live` (default OFF = file absent).

- Flip:     `touch /etc/schema-init/schema-udev.live` + reboot
- Fallback: `rm   /etc/schema-init/schema-udev.live` + reboot

One master flag, not per-capability — the flip is all-or-nothing (you cannot
half-own `/dev`). Inspectable at any time; survives across `kill -HUP` (re-read
only at process start, so a running daemon never changes ownership mid-flight).

---

## E1 — artifact parity gate (no mutation)

**Goal:** prove schema-udev's isolated output is byte-for-byte equivalent to
udevd's real output, across a full boot, before any live write exists.

Mirror the existing `tests/verify_uaccess_live.sh` (forward "only-ours" + reverse
"completeness" against live `getfacl`) for the other two artifact classes:

- **`tests/verify_disk_links_live.sh`**
  - Forward: every `/dev/schema/disk/by-*/NAME` resolves to the same real device
    (by `stat` major:minor) as `/dev/disk/by-*/NAME`.
  - Reverse: every in-scope real `/dev/disk/by-*/NAME` has a matching schema link.
  - In-scope: `by-uuid`, `by-partuuid`, `by-label`, `by-partlabel`, `by-id`,
    `by-diskseq`. `by-path` deferral documented honestly (per commit `e92ecdd`).
- **`tests/verify_db_live.sh`**
  - Wrap the existing read-only `udev-parity` harness as a pass/fail gate: for
    in-scope subsystems, schema's `/run/schema-udev/data` records must reproduce
    every real `/run/udev/data` `E:` key. Nonzero in-scope mismatch = FAIL.

**Gate definition:** E1 green = all three verify scripts PASS on a live boot.
Only then is E3 (the flip) permitted. E1 writes nothing real.

The uaccess gate (`verify_uaccess_live.sh`) already exists and passes; E1 adds the
two missing gates and a top-level `make verify-live` target running all three.

---

## E2 — live-capable binary, `g_live` OFF

**Goal:** the binary gains the ability to own the real resources, but that ability
is unreachable while the sentinel is absent. Reboot-prove flag-OFF is
byte-identical to today's dry-run.

In `dispatch()`, select targets by `g_live` (base dirs chosen once):

| capability      | `g_live` OFF (dry)          | `g_live` ON (live)                    | status |
|-----------------|-----------------------------|---------------------------------------|--------|
| dev symlinks    | `SCHEMA_DEV_DIR` `/dev/schema` | `/dev`                             | **E2** |
| disk links      | `SCHEMA_DISK_DIR` `/dev/schema/disk` | `/dev/disk`                  | **E2** |
| uaccess         | `uaccess_record` only       | `uaccess_record` **+ new `uaccess_apply`** (`acl_set_file` real node) | **E2** |
| udev db         | `SCHEMA_UDEV_DB_DIR` `/run/schema-udev/data` (both modes) | *(deferred)* | **E3** |
| libudev monitor | *(nothing)*                 | `sendto` group 2 via `libudev_frame_build` | **E3** |

**Two capabilities deferred from E2 to E3** (discovered during E2 build — both
are flag-on-only and cannot be integration-tested until the cutover, so landing
them tonight buys nothing while the flag is off):

- **Real `/run/udev/data` write.** Real records carry `S:`/`I:`/`G:`/`Q:`/`V:`
  lines; `udev_db_record_build` emits `E:` only. Writing E:-only to real
  `/run/udev/data` would break libudev's `V:`-version init check and systemd
  `.device` units (keyed on `G:`/`Q:systemd` tags). E1's db gate does **not**
  catch this (it diffs `E:` keys only). Db stays shadow-only in both modes until
  the record format reaches fidelity. This is an E3 precondition.
- **Group-2 libudev rebroadcast.** `libudev_frame_build` is byte-tested, but the
  `sendto` to the multicast group needs privilege + a live consumer to prove, and
  only runs flag-on. Lands at E3 with the db-format work, tested at the flip.

New code introduced by E2 (as built):

1. **`ua_apply_node` / `ua_clear_node`** in `uaccess.h` — the first `acl_set_file`
   calls in the tree. `ua_apply_node` grants `user:<uid>:rw` on a real node,
   preserving other entries and recomputing the mask (idempotent);
   `ua_clear_node` removes the entry (ENOENT-safe). Unit-tested against a scratch
   file with `acl_get_file` readback (`tests/test_uaccess_apply.c`).
2. **`uaccess_apply(seat_path, ev)`** — thin wrapper: builds `/dev/DEVNAME`,
   applies for eligible subsystems / clears otherwise. In live mode dispatch
   **records AND applies** (decision file stays as audit trail).
3. **`g_live` startup read** — `access("/etc/schema-init/schema-udev.live", F_OK)`,
   read once (never on HUP); logged as `mode=LIVE`/`dry-run`.
4. **Real-dir selection** for symlinks (`dev_base`) and disk links (`disk_base`)
   gated by `g_live`. Db real-write and group-2 send are **not** in E2 (deferred
   above).

**Explicit invariant change (called out loudly):** E2 deliberately ends the
"`acl_set_file` == 0" property. The binary *gains* mutation capability. Safety
moves to: (a) flag-OFF default, (b) the deploy parachute, (c) each live path
being independently unit-tested. `verify_uaccess_live.sh`'s binary assertion
changes from "`acl_set_file` absent" to "`acl_set_file` present **and**
`g_live`-gated" (assert no real ACL changed with the sentinel absent).

**Test plan (E2):**
- `make test` green with new `ua_apply_node`/`ua_clear_node` unit tests
  (scratch file only; real `/dev`, `/run/udev`, real ACLs never touched).
- `vmtest` PASS as PID 1.
- Deploy behind parachute; reboot; **flag OFF** → all three E1 verify scripts
  still PASS (flag-off == today's dry-run, proven byte-identical).

**Deploy parachute (tonight):**
- Back up the current inert binary (`b0eaf5a2…`) + `.svc` under
  `scratchpad/udev-E2-parachute/` with a `rollback.sh` (kill proc + restore inert
  binary + `kill -HUP 1`).
- Sentinel is **not** created. `g_live` stays OFF.
- Reboot-prove flag-off dry-run parity holds.

---

## E3 — the cutover (spec only; NOT built tonight)

The flip, gated on E1 green. E3 must first **build the two deferred mechanisms**
(they only become exercisable once udevd is gone):

- **Real `/run/udev/data` writer** — extend `udev_db_record_build` to emit the
  full record (`S:` symlinks, `I:` init usec, `G:`/`Q:systemd` tags, `V:`
  version) so libudev init-checks and systemd `.device` units keep working. Add a
  db-format fidelity gate (compare full record, not just `E:` keys) before the flip.
- **Group-2 rebroadcast** — the `sendto`-group-2 path, verified end-to-end
  against a live libudev consumer once udevd no longer double-broadcasts.

Then the cutover proper:

1. **Sequence udevd out.** The `udev.svc` oneshot shim
   (`/usr/local/sbin/schema-udev`) stops launching `systemd-udevd` +
   `udevadm trigger/settle`. schema-udev's own coldplug (`coldplug_walk_root`)
   becomes the sole coldplug.
2. **Create the sentinel** so `schema-udev.svc` starts in live mode and owns all
   capabilities.
3. **Parachute (next-boot fallback).** If the box boots degraded, recovery is:
   restore the udevd-starting shim + `rm` the sentinel + reboot. Ship this as a
   pre-staged `rollback-flip.sh` before the flip reboot. Consider a boot-count
   watchdog (schema-init auto-removes the sentinel after N failed boots) —
   evaluate during E3 design.
4. **Retirement.** Once several boots hold clean in live mode, remove the udevd
   packages/units entirely. Not part of the flip itself.

**E3 is explicitly deferred.** It requires E1 green across a real boot first.

---

## Out of scope

- Removing/uninstalling `systemd-udevd` (post-E3 retirement).
- hwdb, `by-path` disk links, and the deferred uaccess subsystems
  (dri/usb/hidraw/rfkill/udmabuf/optical) — tracked separately; not gating the flip.
- dbus reclamation (the broader endgame).
