# Recycle-Proof Session Reaping Implementation Plan (Approach A: leader start-time)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `schema-logind`'s session reaper detect a dead leader whose pid has been recycled, by comparing the leader's `/proc` start-time against a baseline recorded at session registration — immune to which cgroup the leader lives in.

**Architecture:** `schema-session-register` records the leader's `/proc/<pid>/stat` field-22 start-ticks in a private `LEADER_STARTTIME` key. `SessionRecord.leader_alive()` reaps when the pid is gone, or when its live start-ticks differ from the stored baseline (a recycled pid). A session with no baseline key (registered before this change) keeps today's pid-existence behavior, so a HUP-deploy cannot false-reap the already-running session.

**Tech Stack:** Python 3 (stdlib only) + POSIX sh, the existing `schema-logind.py` daemon and `schema-session-register` helper, the repo's hand-rolled `check()`-based test runner.

**Spec:** `docs/superpowers/specs/2026-08-27-reap-hole-cgroup-membership-design.md`

**Context — why approach A, not cgroup membership:** An earlier design checked scope-cgroup membership; it was falsified on the live box (a GUI autologin session's leader runs in the `/schema-init/sddm` *service* cgroup, not the session scope — `schema-plasma-autologin.sh:81` puts a child subshell pid in the scope, not the leader). Start-time is independent of cgroup placement. Do not reintroduce a cgroup-membership check.

## Global Constraints

- **Stdlib / POSIX only.** No new Python imports beyond `os` (already imported). Shell stays POSIX sh (the helper is `#!/bin/sh`, `set -u`).
- **Never false-reap.** `leader_alive` returns `True` (alive) whenever it cannot be certain a leader is gone: no `LEADER_STARTTIME` baseline, or the live start-time is unreadable. A leaderless session (no `LEADER` key) still returns `True`.
- **Never block a login.** `schema-session-register` must still exit 0 and print a valid id even if the leader's start-time cannot be read; the `LEADER_STARTTIME` key is simply omitted then.
- **Do not touch** `CreateSession`'s own logic (it execs the helper and inherits the key), the reuse check, `MONOTONIC`, `REALTIME`, or any D-Bus property.
- **Exact integer compare.** `LEADER_STARTTIME` is raw field-22 ticks (integers), compared by equality — no microsecond conversion, no float tolerance.
- **Shell-vs-Python parity.** The shell field-22 extraction and the Python one must yield the identical integer. Verified: strip through the last `') '` (comm may contain spaces/parens), then take the 20th remaining token (Python `.split()[19]`; shell `awk '{print $20}'`).
- Tests are standalone scripts run with `python3 tests/<file>.py` (exit 0/1), NOT pytest.

---

## File Structure

- `scripts/schema-session-register` (POSIX sh) — compute the leader's field-22 ticks; write the `LEADER_STARTTIME` key into the session file (omit on failure). One added function + one conditional line in the file-body block.
- `scripts/schema-logind.py` — add module helper `proc_starttime_ticks()`; rewrite `SessionRecord.leader_alive()`.
- `tests/test_logind_registry.py` — rewrite `test_leader_sweep` for the start-time logic.
- `tests/test_logind_session_alloc.py` — assert the helper writes `LEADER_STARTTIME` matching the leader's field 22.

---

## Task 1: Approach-A reaping (helper key + start-time liveness) with tests

**Files:**
- Modify: `scripts/schema-session-register` — add `leader_starttime()` after the `[ -n "$LEADER" ] || LEADER=$PPID` line (line 70); add a `LEADER_STARTTIME` line inside the session-file `{ … }` block (after the `LEADER=` printf, line 127 — NOT as the block's last line).
- Modify: `scripts/schema-logind.py` — add `proc_starttime_ticks()` next to `proc_start_usec` (after line 198); rewrite `SessionRecord.leader_alive()` (`:470-474`).
- Test: `tests/test_logind_registry.py` — rewrite `test_leader_sweep` (`:155-167`).
- Test: `tests/test_logind_session_alloc.py` — add a `LEADER_STARTTIME` assertion using the existing `register()`/`senv` harness.

**Interfaces:**
- Consumes: module `_int_or(value, default) -> int`; `SessionRecord.data` (dict of session-file keys); the existing `register(*args) -> (stdout, rc)` and `senv` (with `SCHEMA_LOGIND_RUN_DIR=rundir`) in `test_logind_session_alloc.py::main`.
- Produces:
  - `proc_starttime_ticks(pid: int) -> int | None` — module function in `schema-logind.py`: field-22 ticks, or `None` if unreadable.
  - `LEADER_STARTTIME=<int ticks>` — new optional line in `/run/systemd/sessions/<id>`, written by `schema-session-register`.
  - `SessionRecord.leader_alive(self) -> bool` — unchanged signature; start-time semantics.

- [ ] **Step 1: Write the failing registry test**

Replace `test_leader_sweep` (lines 155-167 of `tests/test_logind_registry.py`) with:

```python
def test_leader_sweep():
    clear_sessions()
    my = os.getpid()
    my_start = L.proc_starttime_ticks(my)   # the real field-22 ticks for this test process

    # live leader, baseline matches -> alive
    write_session(5, UID=1000, USER='ajax80', VTNR=1,
                  LEADER=my, LEADER_STARTTIME=my_start)
    # pid absent -> dead regardless of baseline
    write_session(6, UID=1000, USER='ajax80', VTNR=3,
                  LEADER=999999, LEADER_STARTTIME=12345)
    # recycle case: pid alive in /proc, but baseline start-ticks differ -> dead
    write_session(7, UID=1000, USER='ajax80', VTNR=4,
                  LEADER=my, LEADER_STARTTIME=my_start + 1)
    # no baseline key (pre-upgrade session) -> alive if pid exists (prior behavior)
    write_session(8, UID=1000, USER='ajax80', VTNR=5, LEADER=my)
    # no LEADER key at all -> not ours to reap
    write_session(9, UID=1000, USER='ajax80', VTNR=6)

    recs = L.scan_session_files()
    check('sweep: live leader, matching start-ticks -> alive',
          recs['5'].leader_alive() is True)
    check('sweep: absent pid -> dead',
          recs['6'].leader_alive() is False)
    check('sweep: recycled pid (alive, start-ticks differ) -> dead',
          recs['7'].leader_alive() is False)
    check('sweep: no baseline key -> alive if pid exists (backward-compat)',
          recs['8'].leader_alive() is True)
    check('sweep: no LEADER key is not treated as dead',
          recs['9'].leader_alive() is True)
```

- [ ] **Step 2: Run the registry test to verify it fails**

Run: `cd /home/ajax80/projects/schema-init && python3 tests/test_logind_registry.py`
Expected: FAIL — `AttributeError: module ... has no attribute 'proc_starttime_ticks'` (helper not defined yet), or, once that exists, the recycle case fails under the old `isdir` logic. A red bar tied to the new cases.

- [ ] **Step 3: Add `proc_starttime_ticks` to `schema-logind.py`**

Immediately after `proc_start_usec` (after line 198, before `def svc_name`), add:

```python
def proc_starttime_ticks(pid):
    """Field 22 of /proc/<pid>/stat: process start time in clock ticks since
    boot. Unique to a process incarnation, so it distinguishes a live leader
    from a recycled pid. Returns None if the stat cannot be read. comm (field
    2) may contain spaces/parens, so split on the last ') '."""
    try:
        with open('/proc/%d/stat' % pid) as f:
            return int(f.read().rsplit(') ', 1)[1].split()[19])
    except (OSError, IndexError, ValueError):
        return None
```

- [ ] **Step 4: Rewrite `leader_alive`**

Replace `SessionRecord.leader_alive` (`schema-logind.py:470-474`) with:

```python
    def leader_alive(self):
        pid = _int_or(self.data.get('LEADER'), 0)
        if not pid:
            return True     # nothing claimed a leader; not ours to declare dead
        if not os.path.isdir('/proc/%d' % pid):
            return False    # pid is gone outright
        stored = _int_or(self.data.get('LEADER_STARTTIME'), 0)
        if not stored:
            return True     # no baseline recorded (pre-upgrade session): prior behavior
        live = proc_starttime_ticks(pid)
        if live is None:
            return True     # can't read start-time — never false-reap
        return live == stored   # same incarnation; a mismatch means the pid was recycled
```

- [ ] **Step 5: Run the registry test to verify it passes**

Run: `cd /home/ajax80/projects/schema-init && python3 tests/test_logind_registry.py`
Expected: PASS — all checks, exit 0, including the recycle case (`7 -> dead`) and backward-compat (`8 -> alive`).

- [ ] **Step 6: Make `schema-session-register` write `LEADER_STARTTIME`**

In `scripts/schema-session-register`, add this function right after the `[ -n "$LEADER" ] || LEADER=$PPID` line (line 70):

```sh
# The leader's start time in clock ticks (field 22 of /proc/<pid>/stat). schema-
# logind stores this and compares it on every reap sweep to tell a live leader
# from a recycled pid. comm (field 2) can contain spaces and parens, so split on
# the last ') '. Printed empty on any failure — the key is then simply omitted.
leader_starttime() {
    _stat=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
    _rest=${_stat##*') '}
    printf '%s\n' "$_rest" | awk '{print $20}'
}
LEADER_STARTTIME=$(leader_starttime "$LEADER" 2>/dev/null || printf '')
```

Then inside the session-file `{ … } > "$TMP"` block, add this line immediately
after the `printf 'LEADER=%s\n' "$LEADER"` line (line 127) — it must NOT be the
block's last statement, so the trailing `mv` still runs when the key is absent:

```sh
    [ -n "$LEADER_STARTTIME" ] && printf 'LEADER_STARTTIME=%s\n' "$LEADER_STARTTIME"
```

- [ ] **Step 7: Write the helper key-assertion test**

In `tests/test_logind_session_alloc.py::main`, inside the `try:` block (after the existing registration checks, e.g. after the line that checks `'seat file names the active one'` around line 160), add a self-contained check using a live leader pid:

```python
        # --- LEADER_STARTTIME baseline is recorded for the recycle guard ---
        sid_lt, rc_lt = register(
            '--uid', uid, '--user', user, '--seat', 'seat0', '--vtnr', '7',
            '--type', 'tty', '--leader', str(os.getpid()))
        check('register with a live leader exits 0', rc_lt == 0, 'rc=%d' % rc_lt)
        body = open(os.path.join(rundir, 'sessions', sid_lt)).read()
        want = None
        with open('/proc/%d/stat' % os.getpid()) as f:
            want = int(f.read().rsplit(') ', 1)[1].split()[19])
        check('session file records LEADER_STARTTIME',
              ('LEADER_STARTTIME=%d' % want) in body.splitlines(),
              body)
```

(If `os` is not already imported at the top of the file, add `import os` — check first.)

- [ ] **Step 8: Run the session_alloc test to verify it passes**

Run: `cd /home/ajax80/projects/schema-init && python3 tests/test_logind_session_alloc.py`
Expected: PASS, exit 0 — the new `LEADER_STARTTIME` checks included. (This test spins a private dbus + the daemon; it needs `dbus-daemon` on PATH, which the existing suite already relies on.)

- [ ] **Step 9: Run the full logind suite for regressions**

Run:
```bash
cd /home/ajax80/projects/schema-init
python3 tests/test_logind_registry.py && \
python3 tests/test_logind_session_alloc.py && \
python3 tests/test_logind_multisession.py && \
python3 tests/test_logind_vt.py && \
python3 tests/test_logind_create_session.py && \
echo "ALL LOGIND SUITES PASS"
```
Expected: every suite exits 0, final line `ALL LOGIND SUITES PASS`. `test_logind_create_session` byte-compares `CreateSession` output against a direct helper call excluding volatile keys — confirm `LEADER_STARTTIME` does not break that parity (both go through the same helper, so both carry the key; if the parity test explicitly lists compared keys it is unaffected). If it fails on the new key, report it as a finding rather than editing the parity test blindly.

- [ ] **Step 10: Commit**

```bash
cd /home/ajax80/projects/schema-init
git add scripts/schema-logind.py scripts/schema-session-register tests/test_logind_registry.py tests/test_logind_session_alloc.py
git commit -m "fix(logind): recycle-proof session reaping via leader start-time

leader_alive() checked bare /proc/<pid> existence, so a recycled pid kept
a dead session alive until reboot. schema-session-register now records the
leader's /proc start-ticks (field 22) as LEADER_STARTTIME; leader_alive()
reaps when the pid is gone or its live start-ticks differ from the baseline.
Independent of cgroup placement (an earlier scope-membership approach was
falsified: a GUI autologin leader lives in the sddm service cgroup, not the
session scope). Sessions with no baseline key keep prior behavior, so a HUP
deploy cannot false-reap the running session.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01W2ZCLLWDh3K9C1xLJhn7Dd"
```

---

## Task 2: Deploy to blakbox and verify

**Files:** none modified — validates and deploys Task 1.

**Interfaces:**
- Consumes: the committed Task 1 change.
- Produces: a verified-live daemon + helper on blakbox; no code artifacts.

- [ ] **Step 1: Pre-deploy safety check**

Confirm the live session will NOT be false-reaped: it was registered by the old
helper and must have no baseline key.
```bash
SID=$(loginctl list-sessions --no-legend | awk 'NR==1{print $1}')
grep -c '^LEADER_STARTTIME=' /run/systemd/sessions/$SID
```
Expected: `0` — no `LEADER_STARTTIME` key, so `leader_alive` takes the `if not stored: return True` path. If it prints `1`, STOP and report (the session already carries a baseline; verify its value matches the live leader's field 22 before deploying).

- [ ] **Step 2: Deploy both files + HUP**

```bash
TS=$(date +%Y%m%d-%H%M%S)
sudo cp /usr/local/bin/schema-logind.py /usr/local/bin/schema-logind.py.bak-reapA-$TS
sudo cp /usr/local/bin/schema-session-register /usr/local/bin/schema-session-register.bak-reapA-$TS
sudo cp /home/ajax80/projects/schema-init/scripts/schema-logind.py /usr/local/bin/schema-logind.py
sudo cp /home/ajax80/projects/schema-init/scripts/schema-session-register /usr/local/bin/schema-session-register
sudo kill -HUP 1
```
Expected: PID 1 re-execs, the logind stub reloads; the live desktop session (its real pid) is untouched.

- [ ] **Step 3: Verify no false-reap of the live session**

```bash
loginctl list-sessions          # note the id + leader
sleep 5; loginctl list-sessions # must still be there, same id
schema-doctor 2>/dev/null || sudo python3 /home/ajax80/projects/schema-init/scripts/schema-doctor.py
```
Expected: the live session persists across sweeps; `schema-doctor` CLEAN; no "leader is gone — reaping" for the live session in the schema-logind log/journal.

- [ ] **Step 4: Verify a NEW login gets the baseline and reaps on leader death**

Open a fresh session on another VT (getty on e.g. tty2) or a scripted login, confirm its `/run/systemd/sessions/<id>` now carries `LEADER_STARTTIME`, then end it (log out / kill the leader) and confirm the session disappears from `loginctl` within a sweep and its scope cgroup is `rmdir`'d. If no spare VT is convenient, state that the reap-positive path rests on the Task 1 unit test (recycle case) plus this new-session baseline check, and that no-false-reap was confirmed live.

- [ ] **Step 5: Report verification outcome**

State plainly what was verified live (no-false-reap; new sessions carry the baseline) vs. what rests on unit tests (the recycle comparison itself, which is impractical to force with a real recycled pid).

---

## Self-Review

**1. Spec coverage:**
- `LEADER_STARTTIME` written by the helper → Task 1 Steps 6-7. ✓
- `proc_starttime_ticks` + start-time `leader_alive` → Task 1 Steps 3-4. ✓
- Behavior-table cases (match/absent/recycle/no-baseline/unreadable/leaderless) → Task 1 Step 1 tests (5/6/7/8/9); "start-time unreadable → alive" is covered in code (`if live is None`) and is not separately unit-testable without patching `/proc`, noted here as covered-by-construction. ✓
- Deploy-safety (no baseline on the running session) → Task 2 Step 1 + Task 1 test 8. ✓
- Rollout (unit → pre-check → HUP both files → live verify) → Task 1 Step 9 + Task 2. ✓
- Non-goals (no `MONOTONIC`/D-Bus/`CreateSession`-logic change) → Global Constraints; no task touches them. ✓

**2. Placeholder scan:** No TBD/TODO; every code step carries real code. Task 1 Step 9's parity caveat and Task 2 Step 4's spare-VT fallback are bounded verification instructions, not placeholders. ✓

**3. Type consistency:** `proc_starttime_ticks(pid) -> int|None` used in `leader_alive` and mirrored in the tests; `LEADER_STARTTIME` key name identical across the helper, the daemon read (`self.data.get('LEADER_STARTTIME')`), and both tests; shell `awk '{print $20}'` and Python `.split()[19]` both yield field 22 (verified equal). ✓
