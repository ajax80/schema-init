# Recycle-Proof Session Reaping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `schema-logind`'s session reaper detect a dead leader whose pid has been recycled, by checking scope-cgroup membership instead of bare `/proc/<pid>` existence.

**Architecture:** Replace the liveness test in `SessionRecord.leader_alive()`. A session's real leader is a direct member of that session's own `session-<sid>.scope/cgroup.procs`; a recycled pid lives in a different cgroup and can never appear there, and the kernel drops the pid the instant the leader exits. When the scope is unreadable, fall back to today's `os.path.isdir('/proc/<pid>')` so the change is a strict safety superset.

**Tech Stack:** Python 3 (stdlib only), the existing `schema-logind.py` daemon, POSIX cgroup v2 filesystem, the repo's hand-rolled `check()`-based test runner (no pytest fixtures).

**Spec:** `docs/superpowers/specs/2026-08-27-reap-hole-cgroup-membership-design.md`

## Global Constraints

- **No new long-lived file descriptors, no per-session GLib watches.** Reaping stays a per-sweep read on the existing 250 ms VT poll — one file read per session. (Both prior CPU-spin incidents were long-lived-fd shaped.)
- **Never false-reap.** Any case today's code handles correctly must still be handled correctly; unreadable scope falls back to `os.path.isdir`.
- **Stdlib only.** No new imports beyond what `schema-logind.py` already uses (`os`).
- **Test isolation via env overrides.** Tests must never touch live `/run/systemd` or `/sys/fs/cgroup`; they set `SCHEMA_LOGIND_RUN_DIR` and `SCHEMA_CGROUP_ROOT` before importing the module. `CGROUP_ROOT` is read at import time (`schema-logind.py:245`).
- **Do not touch** `schema-session-register`, `CreateSession`, `MONOTONIC`, `REALTIME`, or any D-Bus property.
- Test runner: each test file is a standalone script exiting 0/1; run with `python3 tests/<file>.py`, not `pytest` (some files call `sys.exit`).

---

## File Structure

- `scripts/schema-logind.py` — add module helper `read_cgroup_procs()`; add `SessionRecord._scope_procs()`; rewrite `SessionRecord.leader_alive()`. All three live together because they are one concern (reap liveness).
- `tests/test_logind_registry.py` — add `SCHEMA_CGROUP_ROOT` temp tree + `write_scope()` helper at module top; extend `test_leader_sweep` with the membership cases.

---

## Task 1: Recycle-proof `leader_alive()` via cgroup membership

**Files:**
- Modify: `scripts/schema-logind.py` — add `read_cgroup_procs()` near the other `/proc`/cgroup module helpers (just after `proc_start_usec`, ends line 198); add `_scope_procs()` and rewrite `leader_alive()` on `SessionRecord` (`:470-474`).
- Test: `tests/test_logind_registry.py` — module-top `SCHEMA_CGROUP_ROOT` setup + `write_scope()` helper; extend `test_leader_sweep` (`:155-167`).

**Interfaces:**
- Consumes: module `CGROUP_ROOT` (`schema-logind.py:245`, honors `SCHEMA_CGROUP_ROOT`); `SessionRecord.sid` (str), `SessionRecord.uid` (int); module `_int_or(value, default) -> int`.
- Produces:
  - `read_cgroup_procs(path: str) -> set[int] | None` — module function; the set of pids in a `cgroup.procs` file, or `None` on any `OSError`.
  - `SessionRecord._scope_procs(self) -> set[int] | None` — the pids in this session's scope, or `None` if unreadable.
  - `SessionRecord.leader_alive(self) -> bool` — unchanged signature; new semantics.

- [ ] **Step 1: Write the failing tests**

At the **module top** of `tests/test_logind_registry.py`, add a cgroup temp tree beside the existing `RUNDIR` block (the `SCHEMA_LOGIND_RUN_DIR` setup at lines 20-23), so it is set before the module import at line 30:

```python
CGROOT = tempfile.mkdtemp(prefix='schema-logind-cg-')
os.environ['SCHEMA_CGROUP_ROOT'] = CGROOT
```

Add this helper next to `write_session` (after line 45):

```python
def write_scope(uid, sid, *pids):
    """Create session-<sid>.scope/cgroup.procs under the test cgroup root and
    populate it with the given pids (one per line, as the kernel would)."""
    scope = os.path.join(
        CGROOT, 'user.slice', 'user-%d.slice' % uid, 'session-%s.scope' % sid)
    os.makedirs(scope, exist_ok=True)
    with open(os.path.join(scope, 'cgroup.procs'), 'w') as f:
        f.write(''.join('%d\n' % p for p in pids))
```

Replace the body of `test_leader_sweep` (lines 155-167) with the backward-compat cases plus the membership cases:

```python
def test_leader_sweep():
    clear_sessions()

    # --- backward-compat: no scope dir -> isdir fallback (today's behavior) ---
    write_session(5, UID=1000, USER='ajax80', VTNR=1, LEADER=os.getpid())
    write_session(6, UID=1000, USER='ajax80', VTNR=3, LEADER=999999)
    recs = L.scan_session_files()
    check('sweep(no-scope): live leader survives via isdir fallback',
          recs['5'].leader_alive() is True)
    check('sweep(no-scope): dead leader detected via isdir fallback',
          recs['6'].leader_alive() is False)

    clear_sessions()
    write_session(8, UID=1000, USER='ajax80', VTNR=1)
    recs = L.scan_session_files()
    check('sweep: no LEADER key is not treated as dead',
          recs['8'].leader_alive() is True)

    # --- (a) live leader present in its scope -> alive ---
    clear_sessions()
    write_session(10, UID=1000, USER='ajax80', VTNR=1, LEADER=os.getpid())
    write_scope(1000, 10, os.getpid())
    recs = L.scan_session_files()
    check('sweep(scope): leader listed in scope -> alive',
          recs['10'].leader_alive() is True)

    # --- (b) recycle case: leader pid is alive in /proc but NOT in the scope ---
    # os.getpid() genuinely exists, so today's isdir check would call this
    # alive; membership must call it dead.
    clear_sessions()
    write_session(11, UID=1000, USER='ajax80', VTNR=1, LEADER=os.getpid())
    write_scope(1000, 11, 4242424)          # some other, unrelated pid
    recs = L.scan_session_files()
    check('sweep(scope): recycled pid (alive in /proc, absent from scope) -> dead',
          recs['11'].leader_alive() is False)

    # --- (c) empty scope -> dead ---
    clear_sessions()
    write_session(12, UID=1000, USER='ajax80', VTNR=1, LEADER=os.getpid())
    write_scope(1000, 12)                    # cgroup.procs present but empty
    recs = L.scan_session_files()
    check('sweep(scope): empty cgroup.procs -> dead',
          recs['12'].leader_alive() is False)

    # --- (d) unreadable scope -> isdir fallback (dead here, pid 999999) ---
    clear_sessions()
    write_session(13, UID=1000, USER='ajax80', VTNR=3, LEADER=999999)
    # no write_scope: scope dir absent -> read_cgroup_procs returns None
    recs = L.scan_session_files()
    check('sweep(no-scope): unreadable scope falls back to isdir -> dead',
          recs['13'].leader_alive() is False)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd /home/ajax80/projects/schema-init && python3 tests/test_logind_registry.py`
Expected: FAIL on `sweep(scope): recycled pid ... -> dead` (returns True under the old `isdir` logic) and on the empty-scope case; `AttributeError` is also acceptable if `_scope_procs` doesn't exist yet. The point is a red bar tied to the new cases.

- [ ] **Step 3: Add the `read_cgroup_procs` module helper**

In `scripts/schema-logind.py`, immediately after `proc_start_usec` (after line 198, before `def svc_name`), add:

```python
def read_cgroup_procs(path):
    """The set of pids listed in a cgroup.procs file, or None if it cannot be
    read (missing scope, permission, unexpected layout). None means 'unknown',
    and callers fall back to a /proc existence check rather than reaping."""
    try:
        with open(path) as f:
            return {int(line) for line in f if line.strip().isdigit()}
    except OSError:
        return None
```

- [ ] **Step 4: Add `_scope_procs` and rewrite `leader_alive` on `SessionRecord`**

Replace `SessionRecord.leader_alive` (`schema-logind.py:470-474`) with:

```python
    def _scope_procs(self):
        """Pids in this session's scope cgroup, or None if unreadable. The
        real leader is a direct member of this scope; a recycled pid lives in
        a different cgroup and can never appear here, and the kernel drops the
        pid the instant the leader exits."""
        path = '%s/user.slice/user-%d.slice/session-%s.scope/cgroup.procs' % (
            CGROUP_ROOT, self.uid, self.sid)
        return read_cgroup_procs(path)

    def leader_alive(self):
        pid = _int_or(self.data.get('LEADER'), 0)
        if not pid:
            return True     # nothing claimed a leader; not ours to declare dead
        procs = self._scope_procs()
        if procs is None:
            # No readable scope (never created / cgroup layout differs): fall
            # back to bare pid existence — exactly the prior behavior.
            return os.path.isdir('/proc/%d' % pid)
        return pid in procs
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd /home/ajax80/projects/schema-init && python3 tests/test_logind_registry.py`
Expected: PASS — final line reports all checks passed, exit 0. Confirm the three previously-existing checks (`no-scope` live/dead, no-`LEADER`) still pass alongside the four new membership checks.

- [ ] **Step 6: Run the full Python logind suite for regressions**

Run:
```bash
cd /home/ajax80/projects/schema-init
python3 tests/test_logind_registry.py && \
python3 tests/test_logind_multisession.py && \
python3 tests/test_logind_session_alloc.py && \
python3 tests/test_logind_vt.py && \
python3 tests/test_logind_create_session.py && \
echo "ALL LOGIND SUITES PASS"
```
Expected: every suite exits 0, final line `ALL LOGIND SUITES PASS`. `leader_alive` is exercised by `test_logind_registry` and indirectly by the reaper; none of the others create scopes for their sessions, so they take the `isdir` fallback and must be unaffected.

- [ ] **Step 7: Commit**

```bash
cd /home/ajax80/projects/schema-init
git add scripts/schema-logind.py tests/test_logind_registry.py
git commit -m "fix(logind): recycle-proof session reaping via cgroup membership

leader_alive() checked bare /proc/<pid> existence, so a recycled pid kept
a dead session alive until reboot. Check membership in the session's own
scope cgroup.procs instead — a recycled pid lives in a different cgroup and
cannot appear there. Falls back to isdir when the scope is unreadable, so
this is a strict safety superset.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01W2ZCLLWDh3K9C1xLJhn7Dd"
```

---

## Task 2: Boot-test under QEMU, then deploy to blakbox

**Files:** none modified — this task validates and deploys Task 1.

**Interfaces:**
- Consumes: the committed `leader_alive()` change from Task 1.
- Produces: a verified-live daemon on blakbox; no code artifacts.

- [ ] **Step 1: LIVE vmtest**

Invoke the `schema-vmtest` skill in LIVE mode (boots schema-init as PID 1 under QEMU/KVM). Confirm: the boot reaches a usable state, a session with a real `LEADER` is present and is **not** reaped across multiple 250 ms sweeps (session persists in `loginctl list-sessions`), and `schema-doctor` reports CLEAN.
Expected: no spurious reap; doctor 6/6.

- [ ] **Step 2: Deploy to blakbox via HUP**

Deploy the updated `scripts/schema-logind.py` to its live path and reload PID 1 **without restarting** (schema-init hard rule — `kill -HUP`, never `restart`):

```bash
sudo cp /home/ajax80/projects/schema-init/scripts/schema-logind.py /usr/local/bin/schema-logind.py
sudo kill -HUP 1
```
Expected: PID 1 re-execs, the logind stub reloads, the live desktop session (its real pid) is untouched.

- [ ] **Step 3: Verify a live session survives repeated sweeps**

```bash
loginctl list-sessions
# note the session id + LEADER, wait a few seconds, re-run — it must persist
sleep 5; loginctl list-sessions
schema-doctor    # or: sudo python3 /home/ajax80/projects/schema-init/scripts/schema-doctor.py
```
Expected: the session and its `LEADER` are stable across sweeps; `schema-doctor` 6/6 CLEAN; no "leader is gone — reaping" lines in the schema-logind log for the live session (`grep reaping /var/log/schema-init/*logind* 2>/dev/null` or the journal-sink log).

- [ ] **Step 4: Verify a genuinely dead session IS reaped**

Create a throwaway session, confirm it registers, kill its leader, and confirm it disappears within a sweep. On a single-user desktop the low-risk check is a second VT login (getty on another tty) or a `loginctl` scripted session; if neither is convenient, this step may be satisfied by the Task 1 unit test `(b)`/`(c)` cases plus the vmtest, noted explicitly in the completion report.
Expected: the dead session is removed from `loginctl list-sessions` and its scope cgroup is `rmdir`'d.

- [ ] **Step 5: Report verification outcome**

State plainly what was verified live vs. what rests on the unit tests + vmtest. Do not claim the recycle case was reproduced on hardware unless a real recycled pid was observed (it is impractical to force) — the recycle guarantee is proven by unit test `(b)` and the kernel's cgroup semantics; the live box proves no-false-reap and real-dead-reap.

---

## Self-Review

**1. Spec coverage:**
- New `leader_alive` semantics + `read_cgroup_procs` + `_scope_procs` → Task 1 Steps 3-4. ✓
- Behavior table cases (live-in-scope, recycle, empty, unreadable, leaderless) → Task 1 Step 1 tests (a)/(b)/(c)/(d) + no-`LEADER`. ✓
- No false-reap / isdir fallback → Task 1 Step 4 `if procs is None` + test (d) and no-scope cases. ✓
- Test isolation via `SCHEMA_CGROUP_ROOT` before import → Task 1 Step 1 module-top setup. ✓
- Rollout: unit tests → vmtest → HUP deploy → live verify → dead-session reap → Task 1 Step 6 + Task 2. ✓
- Non-goals (no writer/MONOTONIC/D-Bus change) → enforced by Global Constraints; no task touches them. ✓

**2. Placeholder scan:** No TBD/TODO; every code step carries real code. Task 2 Step 4's fallback wording is a deliberate, bounded verification allowance, not a placeholder. ✓

**3. Type consistency:** `read_cgroup_procs(path) -> set|None`, `_scope_procs(self) -> set|None`, `leader_alive(self) -> bool` used consistently across Steps 3-4 and the tests; `write_scope(uid, sid, *pids)` matches all call sites; `CGROOT`/`SCHEMA_CGROUP_ROOT` and `CGROUP_ROOT` names align with `schema-logind.py:245`. ✓
