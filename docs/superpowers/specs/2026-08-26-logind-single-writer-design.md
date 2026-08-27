# schema-logind: single session writer (Tier-1 keystone)

**Date:** 2026-08-26
**Status:** Design approved (brainstorming); implementation plan pending.
**Scope:** Full unification (not tactical) — one writer of record for logind sessions.

## Problem

Three independent mechanisms currently create a logind session on schema-init,
and they have drifted apart:

1. **`scripts/schema-session-register`** (shell) — declares itself *"the writer
   of record"* for `/run/systemd/sessions/<id>` + the `session-<id>.scope`
   cgroup. Used by the installer GUI autologin
   (`distros/fedora-installer/rail/scripts/schema-plasma-autologin.sh`).
2. **`CreateSession` in `scripts/schema-logind.py`** (Python/dbus) — but it
   writes the session file (`write_session_file`) and scope (`os.makedirs`)
   **itself**, not via the helper. Fired by `pam_systemd` on getty logins.
3. **Legacy `/usr/local/bin/sddm-logged`** — hand-writes a hardcoded session
   `31` with no `LEADER`. **This is what blakbox still runs** (its live session
   shows `LEADER=-` in `loginctl`).

Two full implementations of the same write logic, and blakbox on neither. This
duality is the root of:

- **Bug #1 (orphan-31 / session-registration race):** two writers, hand-written
  ids, no single allocator on the GUI path → duplicate/orphan sessions. The
  `schema-doctor` `session-single` check only *names* this; it does not fix it.
- **The empty-`LEADER` live session:** blakbox's GUI session has no leader pid,
  so the daemon's `LEADER`-alive reap and any future pidfd reaping have nothing
  to watch.

## Goals

- **One writer of record** for `/run/systemd/sessions/<id>` and
  `session-<id>.scope`: `schema-session-register` / `schema-session-unregister`.
- getty (`pam_systemd → CreateSession`) and GUI (DM autologin) sessions are
  **byte-identical**, produced by that one writer.
- **Legacy `sddm-logged` retired**; blakbox and eli both run the unified GUI
  login script.
- `schema-doctor --check` passes **natively** (no hand-written `31`, one session
  per VT), and every GUI session carries a real `LEADER`.

## Non-goals (later tiers, unblocked by this work)

- **pidfd-based reaping** (Tier 2). This keystone gives every session a real
  `LEADER`, which is the precondition, but the pid→pidfd swap is separate.
- **`HandlePowerKey` / `HandleLidSwitch`** (Tier 3, Bug #3).
- **uaccess device ACLs** on session activate (Tier 3; currently covered by
  static `video`/`render`/`audio` group membership).

## Design

### A. The single-writer contract

`schema-session-register` (+ `-unregister`) is the **sole** implementation of the
session lifecycle: *allocate id → write `/run/systemd/sessions/<id>` →
create / tear down `session-<id>.scope`*. No other code writes those paths.

The invariant the helper already guarantees is promoted to a system rule:
**this path must never block a login.** Every failure falls through to the
legacy id and exits 0. `schema-logind` continues to skip an empty (keyless)
session file, so the half-built `noclobber` claim is never visible on the bus.

### B. `CreateSession` / `ReleaseSession` become thin delegators

`CreateSession` keeps only what needs daemon state and hands the write to the
helper:

1. **Keep** the VT-only guard (`vtnr <= 0 → NotSupported`) — sudo/su
   (`class=background*`, no tty) still get no session.
2. **Keep** the existing-VT-session reuse check (needs `self.registry`): if a
   live, non-synthesised session already exists for that VT+uid, return it with
   `existing=True` and make **no** helper call.
3. **Otherwise exec `schema-session-register`** with mapped args
   (`--uid --user --seat --vtnr --type --class --desktop --service --leader`),
   **propagating the daemon's `RUN_DIR` / `CGROUP_ROOT` test-override constants**
   into the helper's `SCHEMA_LOGIND_RUN_DIR` / `SCHEMA_CGROUP_ROOT` env so the
   existing temp-dir tests stay hermetic.
4. Capture the sid from the helper's stdout → `self.registry.sync()` (fires
   `SessionNew` immediately) → return `_session_reply(sid, uid, seat, vtnr,
   False)`.
5. **Delete** the inline `write_session_file`, `os.makedirs(scope)` +
   `cgroup.procs` write, and the Python `alloc_session_id` from this path.

`ReleaseSession` symmetrically execs `schema-session-unregister <sid> <uid>` and
drops its inline `unlink` / `rmdir`.

Result: getty and DM logins bottom out in one writer, byte-identical.

### C. Retire the legacy path; unify the GUI login

`schema-plasma-autologin.sh` becomes the **one** GUI login script. It already:

- calls `schema-session-register` with a full arg set and `--leader $$`;
- falls back to `SID=31` if registration fails (never-block-login);
- makes the scope and moves **only** the plasma subtree into it (`$BASHPID`
  subshell);
- sets the complete session env — `SHELL`, `XDG_SESSION_ID`, `XDG_MENU_PREFIX=
  plasma-`, `XDG_CONFIG_DIRS` (the ksycoca-loop fix), `PLASMA_USE_SYSTEMD_SCOPE=
  0`;
- installs a `release_session` trap (EXIT/HUP/INT/TERM → `schema-session-
  unregister`).

**Retire `/usr/local/bin/sddm-logged`.** blakbox migrates onto the same script:
parameterize the `SCHEMA_*` vars (`UID/USER/HOME/SHELL/SEAT/VTNR/DATA_DIRS`) for
blakbox and wire it into blakbox's DM/autologin invocation. Parity with what
`sddm-logged` provided is already met, so this is wiring + parameterization, not
feature work. Keep `sddm-logged.bak` for one-reboot rollback.

### D. Teardown symmetry + the doctor

Two teardown callers, one helper: the DM script's `release_session` trap and
`ReleaseSession` both exec `schema-session-unregister`. The daemon's
`LEADER`-alive sweep remains the **backstop** for a session whose leader dies
without unregistering.

Acceptance includes **`schema-doctor --check` CLEAN**: the `session-single`
check (Bug #1) passes natively once there is one writer and one session per VT.

### E. Testing + staged rollout

**TDD (extend `tests/test_logind_create_session.py`):**

- `CreateSession` produces a state file + scope **byte-identical** to a direct
  `schema-session-register` call (same keys, `LEADER` set, scope present).
- The reuse path still returns `existing=True` with **no** second write.
- `RUN_DIR` / `CGROUP_ROOT` overrides reach the helper (temp-dir isolation the
  current suite relies on).
- New `ReleaseSession → schema-session-unregister` symmetry test (file gone,
  scope `rmdir`'d).

**vmtest:** `schema-vmtest` LIVE boot — a GUI session comes up with a real
`LEADER`, the polkit agent registers, `schema-doctor` CLEAN.

**Rollout order (hard):**

1. `schema-vmtest` green.
2. **eli** (test mule): reboot, verify `loginctl` shows a real `LEADER`, a GUI
   polkit action raises the password prompt, `schema-doctor --check` CLEAN.
3. **blakbox last** — only after eli is green; `sddm-logged.bak` retained for a
   one-reboot back-out. Honors the rule that blakbox (daily driver / fleet
   brain) is never the first to take an unproven change.

### F. Risks

- **blakbox login regression.** Mitigated by eli-first, the retained backup, and
  the never-block-login fallback (worst case = today's legacy-31 behavior; the
  login still completes).
- **reuse-check race** (two logins, same VT). The helper's `noclobber` alloc is
  already race-safe; the reuse-check is best-effort and cannot wedge a login.

## Affected files (indicative — plan will finalize)

- `scripts/schema-logind.py` — `CreateSession` / `ReleaseSession` delegation;
  remove duplicated write/alloc/teardown.
- `scripts/schema-session-register` / `schema-session-unregister` — confirmed
  sole writer; no behavior change expected beyond being the single caller
  target.
- `distros/**/scripts/schema-plasma-autologin.sh` — the one GUI login script;
  parameterize for blakbox.
- Remove/retire `/usr/local/bin/sddm-logged` (and its repo source, if tracked).
- `tests/test_logind_create_session.py` (+ a release/unregister test).

## Related

- `docs/superpowers/specs/2026-07-27-logind-multi-session-design.md` (the
  multi-session bridge this builds on).
- Memory: `project_schema_polkit_session_gap`, `project_schema_logind_multisession`,
  `project_schema_session_integration`, `project_schema_doctor`.
