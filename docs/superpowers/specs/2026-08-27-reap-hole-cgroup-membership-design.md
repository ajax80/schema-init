# schema-logind: recycle-proof session reaping via cgroup membership

**Date:** 2026-08-27
**Status:** Design approved (brainstorming); implementation plan pending.
**Scope:** One focused correctness fix to `SessionRecord.leader_alive()` in
`scripts/schema-logind.py`. No writer, D-Bus, or property changes.

## Problem

`SessionRegistry` reaps a session when its leader process is gone. The
liveness check is pid existence:

```python
def leader_alive(self):
    pid = _int_or(self.data.get('LEADER'), 0)
    if not pid:
        return True     # nothing claimed a leader; not ours to declare dead
    return os.path.isdir('/proc/%d' % pid)
```

`os.path.isdir('/proc/<pid>')` answers *"does some process hold this pid,"*
not *"is this session's leader still alive."* After the real leader exits and
the kernel recycles its pid to an unrelated process, the directory exists
again, `leader_alive()` returns `True`, and the dead session lingers in
`loginctl` — and its scope cgroup is never `rmdir`'d — until the next boot.

The reaper (`schema-logind.py:1274`, inside the 250 ms VT poll) already carries
the caveat inline: *"this is pid-based, so a recycled pid could keep a dead
session alive until the next boot. Real logind uses a pidfd."*

## Goals

- A session whose leader has exited is reaped even if that pid is later reused.
- No false-reap: a change that is a strict superset of today's safety — any
  case the current code handles correctly, the new code still handles
  correctly.
- No new long-lived file descriptors, no per-session watches in the GLib loop
  (the shape of both prior CPU-spin incidents).
- Cheap enough to run at the existing 250 ms cadence: one small file read per
  session per sweep.

## Non-goals

- **pidfd-based reaping.** Rejected: this codebase already had a pidfd leak
  reach `EMFILE` and busy-spin; one long-lived fd per session inside the GLib
  loop is exactly the shape both CPU-spin incidents took.
- **Start-time / `MONOTONIC` baseline.** A viable alternative (record the
  leader's `/proc/<pid>/stat` field-22 start time and compare on reap), and it
  would carry a `loginctl`-timestamp correctness bonus, but it re-opens the
  meaning of the on-bus `MONOTONIC` property and needs shell↔Python value
  matching with a tolerance. Deliberately not taken; cgroup membership is
  structurally correct with a smaller surface.
- Any change to `schema-session-register`, `CreateSession`, `MONOTONIC`,
  `REALTIME`, or any D-Bus property.

## Design

### The invariant we lean on

Both session writers place the leader pid into that session's own scope cgroup:

- `schema-session-register` creates
  `$CGROOT/user.slice/user-<uid>.slice/session-<sid>.scope` and the caller
  writes the leader into its `cgroup.procs` (the GUI autologin writes its own
  subshell pid; `CreateSession` writes the login pid — `schema-logind.py:1838`).
- The scope exists precisely because `sd_pid_get_session()` is cgroup-based and
  the polkit auth agent cannot register without it.

Two kernel-guaranteed facts make membership an identity-checked liveness test:

1. A process appears in `cgroup.procs` only for the one cgroup it is a **direct
   member** of. A recycled pid belongs to whatever cgroup its new process was
   created in — never this session's scope, because nothing in schema-init
   moves an unrelated process into an existing session scope.
2. The kernel removes a pid from `cgroup.procs` the instant that process exits.

So *"is `LEADER` listed in `session-<sid>.scope/cgroup.procs`"* is true exactly
while this session's original leader is alive.

### New `leader_alive()`

```python
def leader_alive(self):
    pid = _int_or(self.data.get('LEADER'), 0)
    if not pid:
        return True                      # leaderless legacy/synthesised — not ours to reap
    procs = self._scope_procs()          # set[int], or None if unreadable
    if procs is None:
        return os.path.isdir('/proc/%d' % pid)   # scope absent → prior behavior
    return pid in procs
```

Supporting pieces:

- Module helper `read_cgroup_procs(path) -> set[int] | None`: opens `path`,
  returns the pids as a set of ints; returns `None` on any `OSError` (missing
  file, permission, unexpected cgroup layout) and skips non-integer lines
  defensively.
- `SessionRecord._scope_procs(self)`: builds
  `'%s/user.slice/user-%d.slice/session-%s.scope/cgroup.procs' % (CGROUP_ROOT,
  self.uid, self.sid)` and delegates to `read_cgroup_procs`. `CGROUP_ROOT`
  (`schema-logind.py:245`) already honors the `SCHEMA_CGROUP_ROOT` test
  override, so this stays test-isolated.

### Behavior table

| Case | `_scope_procs()` | Result | vs. today |
|------|------------------|--------|-----------|
| No `LEADER` key (legacy/synthesised) | not reached | alive | unchanged |
| Live leader, pid in scope | `{pid, …}` | alive | unchanged (was alive) |
| Leader exited, pid **not** recycled | `set()` w/o pid | dead | unchanged (was dead) |
| Leader exited, pid **recycled** elsewhere | `set()` w/o pid | **dead** | **FIXED** (was falsely alive) |
| Scope never created / unreadable | `None` | `isdir` fallback | unchanged |

### Semantics note (documented, non-occurring in practice)

A pid that is alive in `/proc` but absent from its session's scope is treated
as gone → reaped. The only way that arises other than recycling is an external
actor moving the leader out of its scope, which nothing in schema-init does.
Fail-toward-reap here is correct: a session whose scope no longer contains its
leader has no live leader to mediate its VT.

## Data flow

Unchanged control flow. The reaper list-comprehension at `schema-logind.py:1274`
calls `r.leader_alive()` per record on each 250 ms sweep; only what that method
reads changes — from one `stat('/proc/<pid>')` to one `open()+read()` of the
session scope's `cgroup.procs` (with the `isdir` path as fallback).

## Testing (TDD)

Extend `tests/test_logind_registry.py`:

1. Add an `SCHEMA_CGROUP_ROOT` temp tree at module top, beside the existing
   `SCHEMA_LOGIND_RUN_DIR` setup (env must be set before the module import at
   line 30, since `CGROUP_ROOT` is read at import).
2. A small helper to create a fake scope and write pids into its `cgroup.procs`:
   `write_scope(uid, sid, *pids)`.
3. Extend `test_leader_sweep`:
   - **Backward-compat (no scope):** existing `LEADER=os.getpid()` → alive,
     `LEADER=999999` → dead, no-`LEADER` → alive all keep passing via the
     `isdir` fallback (no scope dir created for them).
   - **(a) live in scope:** `write_scope(1000, 5, os.getpid())` → session 5
     with `LEADER=os.getpid()` → alive.
   - **(b) recycle case:** session with `LEADER=os.getpid()` (a pid that
     genuinely exists in `/proc`) but `write_scope(...)` containing a
     *different* pid → **dead** (the fix; today's `isdir` would call it alive).
   - **(c) empty scope:** `cgroup.procs` present but empty → dead.
   - **(d) unreadable scope:** scope dir absent → `isdir` fallback path taken.

## Rollout

1. Unit tests green (`test_logind_registry` + full `python3` logind suite).
2. `schema-vmtest` LIVE boot: a GUI session comes up with a real `LEADER` and
   is **not** spuriously reaped across many sweeps; `schema-doctor` CLEAN.
3. Deploy to blakbox by `kill -HUP` of PID 1 (never `restart`, per the
   schema-init deploy rule). Verify the live login survives repeated reap
   sweeps and `loginctl list-sessions` stays stable; then confirm a session
   whose leader is killed is reaped within a sweep.

## Affected files

- `scripts/schema-logind.py` — rewrite `SessionRecord.leader_alive()`; add
  `read_cgroup_procs()` (module) and `SessionRecord._scope_procs()`.
- `tests/test_logind_registry.py` — `SCHEMA_CGROUP_ROOT` setup, `write_scope`
  helper, extended `test_leader_sweep`.
