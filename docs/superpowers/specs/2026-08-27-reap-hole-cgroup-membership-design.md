# schema-logind: recycle-proof session reaping via leader start-time

**Date:** 2026-08-27
**Status:** Design approved (approach A, after approach B was falsified on the
live box). Implementation plan updated to match.
**Scope:** `SessionRecord.leader_alive()` in `scripts/schema-logind.py` plus a
new baseline key written by `scripts/schema-session-register`. No D-Bus
property or on-bus `MONOTONIC` change.

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

## Rejected approach: cgroup membership (approach B) — falsified on the live box

The first design checked whether `LEADER` was a member of that session's own
`session-<sid>.scope/cgroup.procs`, on the assumption that the leader is a
direct member of its session scope. **This is false for GUI autologin
sessions, which are the primary case on this fleet.** Verified live on blakbox
before any deploy:

- Session `1`'s leader is `1745` = `schema-plasma-autologin.sh`, running in the
  `/schema-init/sddm` **service** cgroup — not the session scope.
- The scope (`session-1.scope`) holds the desktop payload (`runuser` 1804 →
  `dbus` → plasma, all pids 1804+) but never `1745`.
- Cause is by design: `schema-plasma-autologin.sh:81` registers `--leader $$`
  (the script) but then a **subshell** does `echo $BASHPID > scope/cgroup.procs`
  — a *child* pid joins the scope and execs the desktop, while the recorded
  leader stays in the service cgroup.

So `pid in scope.cgroup.procs` would be `False` for a healthy GUI session and
the reaper would delete it on the first sweep. (getty/`CreateSession` sessions
place the leader pid into the scope at `schema-logind.py:1843`, so B would have
worked *there* — but not for the desktop.) Approach B is abandoned.

## Approach A: leader start-time baseline

Identify the leader by its own process **start-time**, which is immune to
*where* the leader lives. `/proc/<pid>/stat` field 22 is the process start time
in clock ticks since boot; it is unique to a specific process incarnation, so a
recycled pid — even one running in the same or a different cgroup — has a
different start-time than the original leader.

### The baseline key

`schema-session-register` (the single writer of record for session files, which
`CreateSession` now execs) records the leader's start-ticks in a new private
key when it writes the session file:

```
LEADER_STARTTIME=<field-22 ticks of /proc/$LEADER/stat>
```

Raw integer ticks, not microseconds — so the reaper compares by exact integer
equality with no float/rounding fragility. If `/proc/$LEADER/stat` is
unreadable, the key is omitted (never block a login).

### New `leader_alive()`

```python
def leader_alive(self):
    pid = _int_or(self.data.get('LEADER'), 0)
    if not pid:
        return True                       # leaderless legacy/synthesised — not ours to reap
    if not os.path.isdir('/proc/%d' % pid):
        return False                      # pid is gone outright
    stored = _int_or(self.data.get('LEADER_STARTTIME'), 0)
    if not stored:
        return True                       # no baseline (pre-upgrade session) — prior behavior
    live = proc_starttime_ticks(pid)      # field-22 ticks, or None if unreadable
    if live is None:
        return True                       # can't read start-time — never false-reap
    return live == stored                 # same incarnation; mismatch = recycled pid
```

Supporting piece — a module helper returning raw start-ticks (distinct from the
existing `proc_start_usec`, which converts to microseconds):

```python
def proc_starttime_ticks(pid):
    """Field 22 of /proc/<pid>/stat: the process start time in clock ticks
    since boot. Unique to a process incarnation, so it distinguishes a live
    leader from a recycled pid. Returns None if the stat cannot be read."""
    try:
        with open('/proc/%d/stat' % pid) as f:
            return int(f.read().rsplit(') ', 1)[1].split()[19])
    except (OSError, IndexError, ValueError):
        return None
```

### Behavior table

| Case | Result | vs. today |
|------|--------|-----------|
| No `LEADER` key (legacy/synthesised) | alive | unchanged |
| pid gone from `/proc` | dead | unchanged |
| Live leader, start-ticks match stored | alive | unchanged |
| pid recycled (exists, start-ticks differ) | **dead** | **FIXED** (was falsely alive) |
| No `LEADER_STARTTIME` (session predates upgrade) | alive if pid exists | unchanged (graceful) |
| Start-time unreadable | alive if pid exists | unchanged (never false-reap) |

### Deploy safety (why this cannot false-reap the live session)

The session already running when the new daemon is HUP'd in was written by the
old helper and has **no** `LEADER_STARTTIME` key, so `leader_alive()` takes the
`if not stored: return True` path — exactly today's behavior. The recycle
guard becomes active only for sessions registered after the new helper is
deployed. Existing sessions keep prior behavior until the next login; new
sessions are recycle-proof.

## Non-goals

- **pidfd reaping.** Rejected: a prior pidfd leak reached `EMFILE` and
  busy-spun; one long-lived fd per session in the GLib loop is the shape of
  both prior CPU-spin incidents.
- **Overloading the on-bus `MONOTONIC` property.** The baseline is a private
  `LEADER_STARTTIME` key, so `MONOTONIC`/`REALTIME` and every D-Bus property
  keep their current meaning. No `loginctl` timestamp change.
- Any change to `CreateSession`'s own logic (it already execs the helper, so it
  inherits the new key for free), the reuse check, or session allocation.

## Testing (TDD)

**`tests/test_logind_registry.py::test_leader_sweep`** — the pure-logic core:
- No `LEADER` key → alive (unchanged).
- `LEADER=os.getpid()`, `LEADER_STARTTIME` = this process's real field-22 ticks
  → alive (matching baseline).
- `LEADER=os.getpid()`, `LEADER_STARTTIME` = a wrong value (e.g. stored+1) →
  **dead** (the recycle case: pid alive in `/proc` but start-ticks differ).
- `LEADER=999999` (absent) → dead regardless of key.
- `LEADER=os.getpid()`, **no** `LEADER_STARTTIME` key → alive (backward-compat).

**`tests/test_logind_session_alloc.py`** (or wherever the helper's output is
asserted) — `schema-session-register --leader <pid>` writes a
`LEADER_STARTTIME` line whose value equals field 22 of `/proc/<pid>/stat`;
with an unreadable/dead leader pid the key is absent and the script still
exits 0 with a valid id.

Helper: a small `proc_starttime_ticks`-equivalent read in the test to compute
the expected value, so the assertion is exact.

## Rollout

1. Unit tests green (`test_logind_registry` + `test_logind_session_alloc` +
   full `python3` logind suite).
2. Pre-deploy safety check on blakbox: confirm the live session has **no**
   `LEADER_STARTTIME` key (so the new daemon leaves it alive), i.e. the
   `if not stored` path applies.
3. Deploy **both** `scripts/schema-logind.py` and `scripts/schema-session-register`
   to `/usr/local/bin/`; reload PID 1 with `kill -HUP 1` (never `restart`, per
   the schema-init deploy rule).
4. Verify the live session survives repeated reap sweeps and `loginctl` stays
   stable; then confirm a **new** login gets a `LEADER_STARTTIME` key and that a
   session whose leader is killed is reaped within a sweep.

## Affected files

- `scripts/schema-logind.py` — add `proc_starttime_ticks()`; rewrite
  `SessionRecord.leader_alive()`.
- `scripts/schema-session-register` — compute the leader's field-22 ticks and
  write the `LEADER_STARTTIME` key (omit on failure).
- `tests/test_logind_registry.py` — extended `test_leader_sweep`.
- `tests/test_logind_session_alloc.py` — assert the helper writes the key.
