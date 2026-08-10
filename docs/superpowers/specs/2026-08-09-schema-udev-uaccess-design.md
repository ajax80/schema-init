# schema-udev slice D — uaccess ACL manager (dry-run / verify) — design

**Date:** 2026-08-09
**Sub-project:** schema-udev endgame, slice D (udevd/logind-uaccess retirement, read-side).
**Status:** design approved.

## Goal

Reproduce, in schema-udev, the decision of which local-seat user gets a
dynamic `rw` POSIX ACL on `uaccess`-tagged device nodes — WITHOUT applying
real ACLs while systemd-logind is still live. schema-udev records the
intended decision to a shadow area; a gate proves those decisions match the
ACLs logind actually applied. Real ACL application is deferred to cutover
(slice E). This mirrors the shadow-db / shadow-tree discipline used by every
prior slice.

## Background

In systemd, udevd tags a fixed set of device nodes `uaccess`, and
systemd-logind applies a POSIX ACL granting the active local-seat user `rw`
(entry `user:<uid>:rw` plus `mask::rw`), resetting it on device removal or
seat/session change. The active-seat uid is the security-sensitive input.

schema-init already reclaims the seat/session side: `scripts/schema-logind.py`
resolves the active uid (`get_active_uid`, deliberately from root-created
`/run/user/<uid>` — never forgeable `/proc/<pid>/environ`) and projects it to
`/run/systemd/seats/seat0` as `ACTIVE_UID=<uid>`
(`scripts/schema-logind.py:1403`, byte-format identical to systemd's,
including the `# This is private data. Do not parse.` header). Slice D
CONSUMES that projection; it does not re-implement seat tracking.

**Which daemon writes `seat0` during dry-run:** while systemd-logind is
still live (the whole shadow phase), the live `/run/systemd/seats/seat0` may
be written by systemd-logind rather than schema-logind — the two emit the
identical `ACTIVE_UID=<uid>` format, so `uaccess_active_uid` reads it
correctly regardless of author. This is the same "read logind's live file"
posture the parity harness uses for `/run/udev/data`. At cutover (slice E),
schema-logind takes sole ownership of `seat0`. Slice D therefore has no
unmet dependency: the field it reads exists and is written today.

## Scope

**In scope — eligibility predicate `SUBSYSTEM ∈ {sound, video4linux, media}`.**
On the target host these are clean blanket matches: every node in each
subsystem is uaccess-tagged (verified: 15/15 `sound`, 2/2 `video4linux`,
1/1 `media` — 18 of the 24 tagged devices). Eligibility is therefore a
trivial, safe subsystem check with no per-device logic.

**Dry-run only.** schema-udev computes and RECORDS the intended ACL decision
to a shadow area. It NEVER calls `acl_set_file` or mutates a real device
node while logind is live. Turning on real ACL application is a cutover step
(slice E).

**Out of scope (deferred — documented, honest gate):**
- `dri` — NOT a clean blanket: `card1` is tagged but `renderD128` and
  `card0` are not (renderD uses the render group; card0/card1 asymmetry is
  seat-device-assignment logic coupled to logind). Needs per-device / seat
  logic.
- Device-level special cases: `usb` webcam interface (`c189:*` matched at
  device level, not subsystem), `hidraw`, `rfkill`, `udmabuf`, optical
  `sr0`. Each needs a per-device match rule, not a subsystem blanket.
- The 6 deferred devices are excluded from the gate's "missing" check and
  named explicitly; the gate still FAILS on any undocumented residual.
- Real ACL application, and dynamic re-ACL on seat/session change, both
  belong to cutover (slice E).

## Architecture

**New file: `uaccess.h`** — header-only `static inline`, self-contained,
included by `schema-udev.c` (mirrors `disk_links.h`).

### Constants
- `SCHEMA_UACCESS_DIR "/run/schema-udev/uaccess"` — our shadow decision area.
- `SEAT0_PATH "/run/systemd/seats/seat0"` — schema-logind's seat projection,
  read-only. We deliberately parse `ACTIVE_UID=` (schema-init owns this
  format via schema-logind, notwithstanding systemd's "do not parse"
  header comment).

### Functions

```
int uaccess_active_uid(const char *seat_path);
```
Read the file at `seat_path`, return the integer value of the `ACTIVE_UID=`
line. Return `-1` if the file is absent, unreadable, or has no `ACTIVE_UID=`
(no active session → no grant, matching logind revoking ACLs on an inactive
seat).

```
int uaccess_eligible(const struct uevent *ev);
```
Return 1 iff `uevent_get(ev, "SUBSYSTEM")` is one of `sound`,
`video4linux`, `media`; else 0.

```
int uaccess_record(const char *dir, const char *seat_path, const struct uevent *ev);
```
If `uaccess_eligible(ev)` and `uaccess_active_uid(seat_path) >= 0`: write a
shadow record at `<dir>/c<maj>:<min>` (key built from `MAJOR`/`MINOR`;
char-device prefix `c`) containing the intended decision, atomically
(tmp+rename):
```
DEVNODE=/dev/<DEVNAME>
GRANT_UID=<uid>
ACL=user:<uid>:rw
```
Otherwise (ineligible, or no active uid) clear any stale record via
`uaccess_clear`. `mkdir -p` `dir` as needed. **Never touches the real node.**
Returns 0 (best-effort). (`DEVNAME` is a kernel uevent property — always
present on char device nodes, e.g. `DEVNAME=snd/controlC0`, verified in the
sysfs `uevent` file — so it needs no `run_builtins` to populate.)

```
int uaccess_clear(const char *dir, const struct uevent *ev);
```
Unlink `<dir>/c<maj>:<min>` (ignore `ENOENT`). Returns 0.

### Wiring in `schema-udev.c`

In `dispatch()`, alongside the existing disk_links hooks (a device may be
neither block nor uaccess-eligible; the two are independent):
- `add` / `change`: `uaccess_record(SCHEMA_UACCESS_DIR, SEAT0_PATH, ev)`.
- `remove`: `uaccess_clear(SCHEMA_UACCESS_DIR, ev)`.

### Startup wipe

In `main()` before coldplug, recursively remove `SCHEMA_UACCESS_DIR` (its
own subtree only — a flat dir of records) so coldplug rebuilds a clean set,
consistent with the disk_links startup wipe. `/run/schema-udev/data`
(shadow db) and `/dev/schema/disk` (link farm) are separate and untouched by
this wipe.

## Data flow

```
add/coldplug:  uevent → run_builtins → eligible(SUBSYSTEM)?
                                             ↓ yes
                          active_uid = parse ACTIVE_UID from seat0
                                             ↓ (>=0)
                 /run/schema-udev/uaccess/c<maj>:<min>  (intended decision)

remove:        uevent → uaccess_clear → unlink the shadow record

(NO acl_set_file, NO real-node mutation anywhere in slice D)
```

## Error handling

- Not eligible, or `ACTIVE_UID` absent (inactive seat) → clear the record
  (no grant), matching logind's revoke-on-inactive behavior.
- Malformed / missing seat0 → uid `-1` → no grant.
- Best-effort shadow writes; a failure logs to stderr, non-fatal.
- No handling of dynamic seat/session change re-ACL (deferred to cutover) —
  the shadow reflects the active uid at the time of the device event, which
  for a fresh coldplugged daemon equals the current active uid.

## Testing

**Unit — `tests/test_uaccess.c`:**
- `uaccess_eligible`: 1 for `SUBSYSTEM=sound`/`video4linux`/`media`; 0 for
  `dri`, `hidraw`, `block`, and missing SUBSYSTEM.
- `uaccess_active_uid`: temp seat0 with `ACTIVE_UID=1000` → 1000; file with
  no `ACTIVE_UID=` → -1; missing file → -1.
- `uaccess_record`: eligible synth uevent (`SUBSYSTEM=sound`,
  `DEVNAME=snd/controlC0`, `MAJOR=116`, `MINOR=7`) + temp shadow dir + temp
  seat0(1000) → record `c116:7` written with `GRANT_UID=1000` and
  `ACL=user:1000:rw`.
- ineligible uevent, or seat0 without `ACTIVE_UID`, → no record written /
  stale record cleared.
- `uaccess_clear` removes the record; second clear is a no-op.
- **Dry-run proof (structural, enforceable):** the review/gate greps the
  diff — `uaccess.h` must contain no `acl_*` call and no
  `#include <acl/...>` / `<sys/acl.h>`. (A C unit test can't assert "no path
  outside temp touched" without strace; the real guarantee is that the code
  has no ACL-mutation surface at all. The unit test additionally operates
  entirely within temp dirs.)

**Live parity gate — `tests/verify_uaccess_live.sh` (sudo):**
- Spawn a fresh daemon (`rm -rf /run/schema-udev`, start `./schema-udev`),
  let coldplug settle.
- Forward: for each shadow record under `/run/schema-udev/uaccess`, read
  `GRANT_UID` and `DEVNODE`, `getfacl` the real node, assert
  `user:<GRANT_UID>:rw` is present (our decision matches logind's ACL).
- Reverse: enumerate real nodes with a `user:<uid>:rw` ACL whose
  `SUBSYSTEM ∈ {sound, video4linux, media}`; assert each has a shadow
  record (0 missing).
- **"only-ours" (a shadow record for a node logind did NOT grant) must be
  empty** — security-critical; any such record FAILS the gate.
- Deferred subsystems (`dri`/`usb`/`hidraw`/`rfkill`/`udmabuf`/optical) are
  named and excluded from the "missing" check; the gate still FAILS on any
  undocumented residual node.
- Exit non-zero on any mismatch.

**vmtest:** `cd ~/schema-livetest && ./vmtest.sh` must stay green — slice D
adds only shadow-record bookkeeping on device events, no PID-1 path.

## Boundaries (what MUST NOT change)

- `schema-udev.c` netlink group stays `sa.nl_groups = 1`.
- **No `acl_set_file` / `acl_*` mutation call anywhere** — hard dry-run
  boundary; verify absent from the diff. slice D writes only shadow records.
- No writes outside `/run/schema-udev/uaccess`; no mutation of any real
  device node or `/run/udev`.
- `scripts/schema-logind.py` is read-only input (parse `seat0`), not
  modified.
- `udev-parity.h`, `run_builtins`/`ub_select`, disk_links, and the Phase-2
  `symlink=` path all unchanged — slice D is a new, independent surface.

## Success criteria

1. `make test` green, including new `test_uaccess` subtests.
2. Live gate: every shadow decision matches logind's real ACL; every
   in-scope real uaccess node has a shadow record; "only-ours" empty;
   deferred subsystems documented; 0 undocumented residual.
3. No ACL-mutation call and no real-node write in the entire slice.
4. vmtest PASS.
