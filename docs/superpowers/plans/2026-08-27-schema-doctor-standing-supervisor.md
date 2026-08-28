# schema-doctor Standing-Supervisor Promotion — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Promote schema-doctor from a boot-once seam-checker into a standing supervisor that re-runs on a 10-minute timer, backs off flapping heals, and surfaces a visible health signal — without becoming a daemon.

**Architecture:** A delta on the existing one-file engine (`scripts/schema-doctor.py`). PID 1's native repeating interval timer (`on_active_sec`) re-runs the existing one-shot engine every 10 min. A new `--periodic` mode adds flap backoff+escalate (persisted in a JSON state file), a GREEN/AMBER/RED status file, and `notify-send` on transition. The engine, the 6 checks, the SAFE/DEFERRED grading, the back-out discipline, and the boot run are all unchanged.

**Tech Stack:** Python 3 stdlib only; schema-init `.svc` interval timer; nftables/`notify-send`/`setpriv` (util-linux) at runtime.

**Spec:** `docs/superpowers/specs/2026-08-27-schema-doctor-standing-supervisor-design.md` (builds on `2026-08-22-schema-doctor-design.md`)

## Global Constraints

- **Python 3, stdlib only** — matches `schema-doctor.py` / `schema-logind.py`. No third-party imports.
- **Tests are standalone scripts, NOT pytest.** Each test file `importlib`-loads the hyphenated script as module `sd`, runs manual `check(name, ok)` assertions, prints `PASS`/`FAIL`, and `sys.exit(0 if all(results) else 1)`. Run with `python3 tests/<file>.py`. Copy the loader header verbatim from `tests/test_doctor_engine.py`.
- **`DOCTOR_ROOT` fixture rooting** — the module reads `ROOT = os.environ.get("DOCTOR_ROOT", "")` and prefixes every path (`os.path.join(ROOT, "run/...")`). Tests set `os.environ["DOCTOR_ROOT"]` to a temp dir **before** loading the module, and fabricate the tree under it.
- **Prime directive: never leave the box worse than it found it.** Every new path is best-effort: any exception is swallowed → logged → exit 0. `critical=0` on the svc.
- **`.svc` files: one `args=` line per token** — schema-init splits nothing on whitespace.
- **Flap constants:** `FLAP_THRESHOLD = 3`, `FLAP_WINDOW = 1800` (seconds). At most 3 heals per window; the next break after that is CHRONIC and not healed.
- **Status colors:** `GREEN`, `AMBER`, `RED`; overall = worst present (`RED > AMBER > GREEN`).

---

### Task 1: Status colors — `CheckResult.grade`, `result_color()`, `overall_color()`

**Files:**
- Modify: `scripts/schema-doctor.py` (CheckResult dataclass ~line 31; run_checks ~line 65; add color helpers after run_checks)
- Test: `tests/test_doctor_status.py` (create)

**Interfaces:**
- Consumes: existing `CheckResult`, `run_checks`, `SAFE`, `DEFERRED`.
- Produces: `GREEN`, `AMBER`, `RED` (module constants); `result_color(r: CheckResult) -> str`; `overall_color(results: list) -> str`; `CheckResult.grade: str` field; new result state string `"chronic"`.

- [ ] **Step 1: Write the failing test** — `tests/test_doctor_status.py`

```python
#!/usr/bin/env python3
"""Status color mapping + overall rollup for schema-doctor."""
import os, sys, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

CR = sd.CheckResult
check("clean -> GREEN",   sd.result_color(CR("a", "", "clean")) == sd.GREEN)
check("healed -> AMBER",  sd.result_color(CR("a", "", "healed")) == sd.AMBER)
check("chronic -> RED",   sd.result_color(CR("a", "", "chronic")) == sd.RED)
check("reported DEFERRED -> AMBER", sd.result_color(CR("a", "", "reported", grade=sd.DEFERRED)) == sd.AMBER)
check("reported SAFE -> RED",       sd.result_color(CR("a", "", "reported", grade=sd.SAFE)) == sd.RED)
check("overall worst-wins RED", sd.overall_color(
    [CR("a", "", "clean"), CR("b", "", "healed"), CR("c", "", "chronic")]) == sd.RED)
check("overall AMBER when best is healed", sd.overall_color(
    [CR("a", "", "clean"), CR("b", "", "healed")]) == sd.AMBER)
check("overall GREEN all clean", sd.overall_color([CR("a", "", "clean")]) == sd.GREEN)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_status.py`
Expected: FAIL / AttributeError — `result_color`/`GREEN` not defined, `CheckResult` has no `grade`.

- [ ] **Step 3: Implement**

In `scripts/schema-doctor.py`, after the `SAFE, DEFERRED = ...` line (~20) add:

```python
GREEN, AMBER, RED = "GREEN", "AMBER", "RED"
```

Add a `grade` field to `CheckResult` (after `action`):

```python
    action: str = ""
    grade: str = SAFE
```

In `run_checks`, pass `grade=c.grade` to **every** `CheckResult(...)` construction (the 6 sites: aborted-not-run, clean, reported-not-heal, collateral-rollback, healed, heal-failed). Example for the clean site:

```python
            results[c.name] = CheckResult(c.name, c.summary, "clean", grade=c.grade)
```

After `run_checks` (before `REGISTRY = []`), add:

```python
def result_color(r):
    if r.state == "clean":
        return GREEN
    if r.state == "healed":
        return AMBER
    if r.state == "chronic":
        return RED
    return AMBER if r.grade == DEFERRED else RED   # reported

def overall_color(results):
    rank = {GREEN: 0, AMBER: 1, RED: 2}
    worst = GREEN
    for r in results:
        c = result_color(r)
        if rank[c] > rank[worst]:
            worst = c
    return worst
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_status.py` → Expected: all PASS, exit 0.
Regression: `python3 tests/test_doctor_engine.py` → still all PASS (grade defaults, existing sites unaffected).

- [ ] **Step 5: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_status.py
git commit -m "feat(doctor): status colors — CheckResult.grade + result/overall_color"
```

---

### Task 2: `FlapState` — the backoff state machine

**Files:**
- Modify: `scripts/schema-doctor.py` (add after `overall_color`, before `REGISTRY`)
- Test: `tests/test_doctor_flap.py` (create)

**Interfaces:**
- Consumes: `ROOT`, `GREEN`; `json`, `time`, `os`.
- Produces: `FLAP_THRESHOLD`, `FLAP_WINDOW`; `_boot_id() -> str`; `FlapState` with classmethod `load()`, methods `save(now=None)`, `should_heal(name, now) -> bool`, `record_heal(name, now)`, `recovered(name)`, `is_chronic(name) -> bool`, `_c(name) -> dict`; instance attr `.data` (dict with keys `version, boot_id, last_overall, last_run, checks`).

- [ ] **Step 1: Write the failing test** — `tests/test_doctor_flap.py`

```python
#!/usr/bin/env python3
"""Flap backoff/escalate state machine for schema-doctor."""
import os, sys, json, tempfile, importlib.util
TMP = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = TMP
os.makedirs(os.path.join(TMP, "proc/sys/kernel/random"), exist_ok=True)
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-A\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

# 3 heals allowed in-window, 4th blocked + chronic
fs = sd.FlapState.load()
t = 1000
oks = []
for i in range(3):
    oks.append(fs.should_heal("x", t + i))       # True, True, True
    if oks[-1]:
        fs.record_heal("x", t + i)
blocked = fs.should_heal("x", t + 3)              # False -> chronic
check("first 3 heals allowed", oks == [True, True, True])
check("4th blocked", blocked is False)
check("marked chronic", fs.is_chronic("x") is True)

# recovery clears chronic + history
fs.recovered("x")
check("recovered clears chronic", fs.is_chronic("x") is False)
check("recovered clears heals", fs._c("x")["heals"] == [])

# window pruning: old heals age out
fs2 = sd.FlapState.load()
fs2._c("y")["heals"] = [10, 20, 30]               # far older than now
check("prune re-allows after window", fs2.should_heal("y", 10 + sd.FLAP_WINDOW + 1) is True)

# persistence + corrupt-file tolerance
fs.record_heal("z", 5000); fs.save(now=5000)
open(os.path.join(TMP, "var/lib/schema-init/doctor-state"), "w").write("{ not json")
fs3 = sd.FlapState.load()
check("corrupt state -> empty, no crash", fs3.data["checks"] == {})

# boot_id change resets heals
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-B\n")
fs4 = sd.FlapState(json.loads(json.dumps(
    {"version": 1, "boot_id": "boot-A", "last_overall": sd.GREEN, "last_run": 0,
     "checks": {"x": {"heals": [1, 2, 3], "chronic": True, "last_state": sd.RED}}})))
open(os.path.join(TMP, "var/lib/schema-init/doctor-state"), "w").write(json.dumps(fs4.data))
fs5 = sd.FlapState.load()
check("boot change resets heals", fs5._c("x")["heals"] == [])
check("boot change clears chronic", fs5._c("x")["chronic"] is False)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_flap.py`
Expected: FAIL — `FlapState` not defined.

- [ ] **Step 3: Implement** — add after `overall_color`:

```python
FLAP_THRESHOLD = 3
FLAP_WINDOW = 1800   # seconds


def _boot_id():
    try:
        return open(os.path.join(ROOT, "proc/sys/kernel/random/boot_id")).read().strip()
    except OSError:
        return ""


class FlapState:
    PATH = "var/lib/schema-init/doctor-state"

    def __init__(self, data):
        self.data = data

    @classmethod
    def load(cls):
        try:
            data = json.load(open(os.path.join(ROOT, cls.PATH)))
            if not isinstance(data, dict):
                data = {}
        except (OSError, ValueError):
            data = {}
        data.setdefault("version", 1)
        data.setdefault("last_overall", GREEN)
        data.setdefault("last_run", 0)
        data.setdefault("checks", {})
        bid = _boot_id()
        if data.get("boot_id") != bid:            # fresh boot -> fresh flap history
            data["boot_id"] = bid
            for c in data["checks"].values():
                c["heals"] = []
                c["chronic"] = False
        return cls(data)

    def save(self, now=None):
        self.data["last_run"] = int(now if now is not None else time.time())
        p = os.path.join(ROOT, self.PATH)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p + ".tmp", "w") as fh:
            json.dump(self.data, fh)
        os.replace(p + ".tmp", p)
        os.chmod(p, 0o644)

    def _c(self, name):
        return self.data["checks"].setdefault(
            name, {"heals": [], "chronic": False, "last_state": GREEN})

    def should_heal(self, name, now):
        c = self._c(name)
        c["heals"] = [t for t in c["heals"] if now - t < FLAP_WINDOW]
        if len(c["heals"]) >= FLAP_THRESHOLD:
            c["chronic"] = True
            return False
        return True

    def record_heal(self, name, now):
        self._c(name)["heals"].append(int(now))

    def recovered(self, name):
        c = self._c(name)
        c["heals"] = []
        c["chronic"] = False

    def is_chronic(self, name):
        return self._c(name).get("chronic", False)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_flap.py` → all PASS, exit 0.

- [ ] **Step 5: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_flap.py
git commit -m "feat(doctor): FlapState backoff/escalate state file"
```

---

### Task 3: Flap-aware healing in `run_checks`

**Files:**
- Modify: `scripts/schema-doctor.py` (`run_checks` ~line 65)
- Test: `tests/test_doctor_flap.py` (extend)

**Interfaces:**
- Consumes: `FlapState`, `FLAP_THRESHOLD`, `FLAP_WINDOW`.
- Produces: `run_checks(checks, heal, force, flap=None, now=None)` — when `flap` is a `FlapState`, SAFE auto-heals consult it: chronic → result state `"chronic"` (not healed); successful heal → `flap.record_heal`; clean detect → `flap.recovered`. `flap=None` = unchanged behavior.

- [ ] **Step 1: Write the failing test** — append to `tests/test_doctor_flap.py` before the final print:

```python
class Flaky(sd.Check):
    """SAFE check that heals (verify passes) but re-breaks before the next run."""
    def __init__(self):
        self.name = self.summary = "flaky"; self.grade = sd.SAFE; self._broken = True
    def detect(self):
        return sd.Finding("broke again") if self._broken else None
    def heal(self, f): self._broken = False          # verify() -> detect() is None -> True

fs6 = sd.FlapState.load()
states = []
for i in range(4):
    c = Flaky()
    r = sd.run_checks([c], heal=True, force=set(), flap=fs6, now=9000 + i)[0]
    states.append(r.state)
check("periodic: heals 3x then chronic", states == ["healed", "healed", "healed", "chronic"], str(states))

# boot mode (flap=None) never goes chronic — heals every time
boot_states = [sd.run_checks([Flaky()], heal=True, force=set(), now=9000 + i)[0].state for i in range(4)]
check("boot mode: always heals, never chronic", boot_states == ["healed"] * 4, str(boot_states))
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_flap.py`
Expected: FAIL — `run_checks()` got an unexpected keyword argument `flap`.

- [ ] **Step 3: Implement** — modify `run_checks`:

Change the signature and seed `now`:

```python
def run_checks(checks, heal, force, flap=None, now=None):
    if now is None:
        now = time.time()
    results = {}
    order = []
    clean = []
    aborted = False
```

In the clean branch (`if f is None:`), after `clean.append(c)`:

```python
        if f is None:
            results[c.name] = CheckResult(c.name, c.summary, "clean", grade=c.grade)
            clean.append(c)
            if flap is not None:
                flap.recovered(c.name)
            continue
```

After `will_heal = ...` and the `if not will_heal:` block, insert the flap gate before `snap = c.snapshot()`:

```python
        if flap is not None and c.grade == SAFE and c.name not in force:
            if not flap.should_heal(c.name, now):
                results[c.name] = CheckResult(
                    c.name, c.summary, "chronic", c.explain(f), f.oracle_said,
                    f"chronic — {FLAP_THRESHOLD}+ heals in {FLAP_WINDOW // 60}m, not re-healing",
                    grade=c.grade)
                continue
        snap = c.snapshot()
```

In the healed branch (`if c.verify():`), record the heal:

```python
        if c.verify():
            results[c.name] = CheckResult(c.name, c.summary, "healed",
                                          c.explain(f), f.oracle_said, "healed", grade=c.grade)
            clean.append(c)
            if flap is not None:
                flap.record_heal(c.name, now)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_flap.py` → all PASS.
Regression: `python3 tests/test_doctor_engine.py` → all PASS (flap defaults None).

- [ ] **Step 5: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_flap.py
git commit -m "feat(doctor): flap-aware SAFE healing in run_checks"
```

---

### Task 4: Status file — render, write, `--status` read

**Files:**
- Modify: `scripts/schema-doctor.py` (add after `write_report` ~line 447)
- Test: `tests/test_doctor_status.py` (extend)

**Interfaces:**
- Consumes: `result_color`, `overall_color`, `ROOT`, `time`, `json`.
- Produces: `render_status(results, mode) -> str`; `status_json(results, mode) -> str`; `write_status(text) -> path` (writes `run/schema-init/doctor-status`, 0644); `read_status() -> str`.

- [ ] **Step 1: Write the failing test** — append to `tests/test_doctor_status.py` before the final print (note: this file has no DOCTOR_ROOT yet — add it at the very top, before the import, so `write_status` lands in a temp dir):

At the very top of the file, before `REPO = ...`:

```python
import tempfile
os.environ["DOCTOR_ROOT"] = tempfile.mkdtemp()
```

Appended assertions:

```python
rs = [CR("card-input-acl", "gpu", "clean"),
      CR("powerdevil-running", "power", "chronic", "died", "", "chronic — 3+"),
      CR("session-single", "sess", "reported", "orphan #31", "", "deferred", grade=sd.DEFERRED)]
txt = sd.render_status(rs, "periodic")
check("status header has overall RED", txt.splitlines()[0].startswith("schema-doctor: RED"))
check("status lists each check", "card-input-acl" in txt and "powerdevil-running" in txt)
p = sd.write_status(txt)
check("status file written", os.path.exists(p))
check("read_status round-trips", sd.read_status() == txt)
j = json.loads(sd.status_json(rs, "periodic"))
check("json overall RED", j["overall"] == sd.RED)
check("json mode periodic", j["mode"] == "periodic")
check("json 3 checks", len(j["checks"]) == 3)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_status.py`
Expected: FAIL — `render_status` not defined.

- [ ] **Step 3: Implement** — add after `write_report`:

```python
def render_status(results, mode):
    ov = overall_color(results)
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    lines = [f"schema-doctor: {ov}   {ts}   mode={mode}"]
    for r in results:
        tail = r.action or r.detail or ""
        lines.append(f"  {result_color(r):<6} {r.name:<22} {tail}")
    return "\n".join(lines) + "\n"


def status_json(results, mode):
    return json.dumps({
        "overall": overall_color(results),
        "mode": mode,
        "ts": int(time.time()),
        "checks": [{"name": r.name, "color": result_color(r), "state": r.state,
                    "detail": r.detail, "action": r.action} for r in results],
    }, indent=2)


def write_status(text):
    d = os.path.join(ROOT, "run/schema-init")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "doctor-status")
    with open(p, "w") as fh:
        fh.write(text)
    os.chmod(p, 0o644)
    return p


def read_status():
    try:
        return open(os.path.join(ROOT, "run/schema-init/doctor-status")).read()
    except OSError:
        return "schema-doctor: no status yet (run --heal or wait for the periodic timer)\n"
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_status.py` → all PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_status.py
git commit -m "feat(doctor): GREEN/AMBER/RED status file + --status reader"
```

---

### Task 5: Notify targeting + transition notifications

**Files:**
- Modify: `scripts/schema-doctor.py` (add after `read_status`)
- Test: `tests/test_doctor_notify.py` (create)

**Interfaces:**
- Consumes: `active_uid`, `_proc_table`, `result_color`, `overall_color`, `GREEN`, `RED`, `subprocess`.
- Produces: `active_session_env() -> (uid|None, env|None)`; `_notify_argv(uid, summary, body) -> list`; `notify_send(uid, env, summary, body)`; `notify_transitions(results, flap, enabled)` (updates `flap` `last_state`/`last_overall` always; fires `notify_send` only when `enabled` and a transition occurred).

- [ ] **Step 1: Write the failing test** — `tests/test_doctor_notify.py`

```python
#!/usr/bin/env python3
"""Notify targeting + edge-triggered transitions for schema-doctor."""
import os, sys, tempfile, importlib.util
TMP = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = TMP
# fabricate an active session (uid 1000) and a GUI proc carrying the /tmp/dbus bus
os.makedirs(os.path.join(TMP, "run/systemd/sessions"), exist_ok=True)
open(os.path.join(TMP, "run/systemd/sessions/1"), "w").write("UID=1000\nACTIVE=1\n")
pdir = os.path.join(TMP, "proc/999"); os.makedirs(pdir, exist_ok=True)
open(os.path.join(pdir, "cmdline"), "wb").write(b"plasmashell\0")
open(os.path.join(pdir, "environ"), "wb").write(
    b"DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-abc\0WAYLAND_DISPLAY=wayland-0\0")
os.makedirs(os.path.join(TMP, "proc/sys/kernel/random"), exist_ok=True)
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-A\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

CR = sd.CheckResult

uid, env = sd.active_session_env()
check("finds active uid", uid == 1000, str(uid))
check("harvests /tmp/dbus bus", env and env.get("DBUS_SESSION_BUS_ADDRESS") == "unix:path=/tmp/dbus-abc")
check("notify argv targets uid + app", sd._notify_argv(1000, "S", "B")[:6] ==
      ["setpriv", "--reuid", "1000", "--regid", "1000", "--clear-groups"])

# capture notify_send calls without running anything
sent = []
sd.notify_send = lambda uid, env, summary, body: sent.append((summary, body))

fs = sd.FlapState.load()
# RED transition (chronic check), prev GREEN -> one notify
red = [CR("powerdevil-running", "power", "chronic", "died", "", "chronic")]
sd.notify_transitions(red, fs, enabled=True)
check("RED transition notifies once", len(sent) == 1, str(sent))
# same RED again -> no new notify (edge-triggered)
sd.notify_transitions(red, fs, enabled=True)
check("no repeat notify while still RED", len(sent) == 1)
# recovery to GREEN -> 'all clear'
sent.clear()
green = [CR("powerdevil-running", "power", "clean")]
sd.notify_transitions(green, fs, enabled=True)
check("recovery to GREEN notifies", any("clear" in s.lower() for s, _ in sent), str(sent))
# disabled -> no sends but bookkeeping still updates
sent.clear()
fs2 = sd.FlapState.load()
sd.notify_transitions(red, fs2, enabled=False)
check("disabled suppresses send", sent == [])
check("disabled still records last_state", fs2._c("powerdevil-running")["last_state"] == sd.RED)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_notify.py`
Expected: FAIL — `active_session_env` not defined.

- [ ] **Step 3: Implement** — add after `read_status`:

```python
def active_session_env():
    uid = active_uid()
    for p in _proc_table():
        if p["env"].get("DBUS_SESSION_BUS_ADDRESS"):
            return uid, p["env"]
    return uid, None


def _notify_argv(uid, summary, body):
    return ["setpriv", "--reuid", str(uid), "--regid", str(uid), "--clear-groups",
            "notify-send", "-a", "schema-doctor", summary, body]


def notify_send(uid, env, summary, body):
    if uid is None or not env or not env.get("DBUS_SESSION_BUS_ADDRESS"):
        return
    child = {"DBUS_SESSION_BUS_ADDRESS": env["DBUS_SESSION_BUS_ADDRESS"],
             "DISPLAY": env.get("DISPLAY", ""),
             "WAYLAND_DISPLAY": env.get("WAYLAND_DISPLAY", ""),
             "XDG_RUNTIME_DIR": env.get("XDG_RUNTIME_DIR", f"/run/user/{uid}"),
             "PATH": "/usr/bin:/bin"}
    try:
        subprocess.run(_notify_argv(uid, summary, body), env=child,
                       timeout=5, check=False)
    except (OSError, subprocess.SubprocessError):
        pass


def notify_transitions(results, flap, enabled):
    events = []
    for r in results:
        col = result_color(r)
        prev = flap._c(r.name).get("last_state", GREEN)
        if col == RED and prev != RED:
            events.append((f"schema-doctor: {r.name}",
                           r.action or r.detail or "needs attention"))
        flap._c(r.name)["last_state"] = col          # bookkeeping, always
    new_overall = overall_color(results)
    if new_overall == GREEN and flap.data.get("last_overall", GREEN) != GREEN:
        events.append(("schema-doctor: all clear", "all seams healthy again"))
    flap.data["last_overall"] = new_overall
    if enabled and events:
        uid, env = active_session_env()
        for summ, body in events:
            notify_send(uid, env, summ, body)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_notify.py` → all PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_notify.py
git commit -m "feat(doctor): notify-send on transition, active-session env harvest"
```

---

### Task 6: Wire `--periodic` / `--status` into `main()` + `notify=` config

**Files:**
- Modify: `scripts/schema-doctor.py` (`read_config` ~401, `_safe_detect_all` ~463, `main` ~471)
- Test: `tests/test_doctor_mode.py` (create)

**Interfaces:**
- Consumes: everything above.
- Produces: `read_config() -> (heal, disabled, notify)`; `_safe_detect_all(checks, heal, force, flap=None)`; `main` handling `--periodic` and `--status`.

- [ ] **Step 1: Write the failing test** — `tests/test_doctor_mode.py`

```python
#!/usr/bin/env python3
"""End-to-end mode wiring: --periodic writes state+status, --status reads, --heal is boot."""
import os, sys, tempfile, importlib.util
TMP = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = TMP
os.makedirs(os.path.join(TMP, "proc/sys/kernel/random"), exist_ok=True)
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-A\n")
os.makedirs(os.path.join(TMP, "etc/schema-init"), exist_ok=True)
open(os.path.join(TMP, "etc/schema-init/doctor.conf"), "w").write("notify=no\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

# empty REGISTRY -> all-clean run, deterministic
sd.REGISTRY[:] = []

check("read_config parses notify", sd.read_config() == (True, set(), False))

rc = sd.main(["--heal", "--periodic"])
check("periodic exits 0", rc == 0)
check("periodic writes state file", os.path.exists(os.path.join(TMP, "var/lib/schema-init/doctor-state")))
check("periodic writes status file", os.path.exists(os.path.join(TMP, "run/schema-init/doctor-status")))

import io, contextlib
buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    sd.main(["--status"])
check("--status prints GREEN (empty registry)", "GREEN" in buf.getvalue())

# boot mode writes status but not a fresh state file: remove state, run --heal, assert none created
os.remove(os.path.join(TMP, "var/lib/schema-init/doctor-state"))
sd.main(["--heal"])
check("boot mode writes no state file", not os.path.exists(os.path.join(TMP, "var/lib/schema-init/doctor-state")))
check("boot mode still writes status", os.path.exists(os.path.join(TMP, "run/schema-init/doctor-status")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_mode.py`
Expected: FAIL — `read_config` returns a 2-tuple / no `--periodic` arg.

- [ ] **Step 3: Implement**

`read_config` — add `notify` (default True) and return it:

```python
def read_config():
    heal = True
    disabled = set()
    notify = True
    path = os.path.join(ROOT, "etc/schema-init/doctor.conf")
    try:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                k, _, v = line.partition("=")
                k, v = k.strip(), v.strip()
                if k == "heal" and v.lower() in ("no", "off", "0", "false"):
                    heal = False
                elif k == "disable":
                    disabled |= {x.strip() for x in v.split(",") if x.strip()}
                elif k == "notify" and v.lower() in ("no", "off", "0", "false"):
                    notify = False
    except FileNotFoundError:
        pass
    return heal, disabled, notify
```

`_safe_detect_all` — thread `flap`:

```python
def _safe_detect_all(checks, heal, force, flap=None):
    try:
        return run_checks(checks, heal, force, flap=flap)
    except Exception as e:
        return [CheckResult("schema-doctor", "internal", "reported",
                            f"doctor aborted: {e}", "", "logged, exit 0")]
```

`main` — add the two flags, unpack the 3-tuple, handle `--status`, and wire flap/status/notify:

```python
    ap.add_argument("--periodic", action="store_true", help="flap-aware run + notify (timer)")
    ap.add_argument("--status", action="store_true", help="print the last health status")
    args = ap.parse_args(argv)

    cfg_heal, disabled, cfg_notify = read_config()
    checks = [c for c in REGISTRY if c.name not in disabled]

    if args.status:
        print(read_status())
        return 0
```

(Leave the existing `--explain` block as-is, after this.) Then replace the tail (from `heal = ...` onward):

```python
    if args.wait:
        wait_for_session(args.wait)

    heal = (args.heal or (not args.check and not args.dry_run)) and cfg_heal and not args.dry_run
    flap = FlapState.load() if args.periodic else None
    results = _safe_detect_all(checks, heal, set(args.force), flap)

    mode = "periodic" if args.periodic else ("boot" if args.wait else "manual")
    try:
        write_status(render_status(results, mode))
    except Exception:
        pass
    if flap is not None:
        try:
            notify_transitions(results, flap, cfg_notify)
            flap.save()
        except Exception:
            pass

    out = render_json(results) if args.json else render_report(results)
    write_report(render_report(results))
    print(out)
    return 0
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_mode.py` → all PASS.
Regression — run the whole doctor suite:
```bash
for t in engine acl cli ksycoca power powerdevil rearm session vt status flap notify mode; do
  python3 tests/test_doctor_$t.py >/dev/null 2>&1 && echo "PASS $t" || echo "FAIL $t"
done
```
Expected: every line `PASS` (note: `acl`/`session`/etc. may `skip` if they need root — treat their own exit 0 as pass).

- [ ] **Step 5: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_mode.py
git commit -m "feat(doctor): --periodic/--status wiring + notify= config"
```

---

### Task 7: Periodic timer service + live deploy on blakbox

**Files:**
- Create: `distros/fedora-installer/rail/services/schema-doctor-periodic.svc`
- (Deploy targets, not repo files: `/usr/local/bin/schema-doctor`, `/etc/schema-init/services/schema-doctor-periodic.svc`)

**Interfaces:**
- Consumes: the finished `schema-doctor` with `--periodic`.
- Produces: a registered, armed `schema-doctor-periodic` interval timer.

- [ ] **Step 1: Create the timer svc**

`distros/fedora-installer/rail/services/schema-doctor-periodic.svc`:

```
name=schema-doctor-periodic
exec=/usr/local/bin/schema-doctor
args=--heal
args=--periodic
on_boot_sec=600
on_active_sec=600
needs_root=1
critical=0
```

- [ ] **Step 2: Commit the svc**

```bash
git add distros/fedora-installer/rail/services/schema-doctor-periodic.svc
git commit -m "feat(doctor): schema-doctor-periodic interval-timer svc (10 min)"
```

- [ ] **Step 3: Deploy the updated engine + svc to blakbox (live, no reboot)**

The `schema-doctor` binary is a client script (no PID 1 involvement); the svc is **new** (not a changed one), so `schema-ctl add` loads it live — the integrity-lock only blocks *changed* svcs.

```bash
sudo install -m 0755 scripts/schema-doctor.py /usr/local/bin/schema-doctor
sudo install -m 0644 distros/fedora-installer/rail/services/schema-doctor-periodic.svc \
     /etc/schema-init/services/schema-doctor-periodic.svc
sudo schema-ctl add /etc/schema-init/services/schema-doctor-periodic.svc
```

- [ ] **Step 4: Live-verify on blakbox**

```bash
sudo schema-ctl status | grep schema-doctor-periodic     # registered, timer state
sudo schema-doctor --heal --periodic                     # one manual run, exits 0
schema-doctor --status                                   # prints GREEN/AMBER/RED table
cat /var/lib/schema-init/doctor-state                     # state file present, JSON
```

Expected: svc registered; the run exits 0; `--status` shows a color table for the 6 live checks; state file is valid JSON with a `boot_id` and `checks`. If any live check reports RED/CHRONIC, that is a real finding to record — not a plan failure.

- [ ] **Step 5: Commit (nothing new to commit — deploy only).** Record the live-verify outcome in the PR/close-out notes.

---

## Self-Review

**Spec coverage:**
- Periodic execution / timer → Task 7 (svc) + Task 6 (`--periodic`). ✓
- Boot run unchanged → Tasks 3 & 6 keep `flap=None` path identical; regression asserted. ✓
- Flap backoff + escalate + state file → Tasks 2 & 3. ✓
- Reboot reset via `boot_id` → Task 2 (`load()`), tested. ✓
- Status signal GREEN/AMBER/RED + `--status` + `--json` → Tasks 1 & 4. ✓
- notify-send on transition + `/proc/environ` bus harvest + best-effort → Task 5. ✓
- notify on recovery to GREEN → Task 5 (`notify_transitions`), tested. ✓
- `notify=` config → Task 6. ✓
- Safety: exception-swallowing, corrupt-state tolerance, best-effort notify → Tasks 2/5/6. ✓
- Testing: flap / status / notify / mode → Tasks 2–6, standalone-script convention. ✓

**Placeholder scan:** no TBD/TODO; every code step shows real code; no "similar to Task N". ✓

**Type consistency:** `run_checks(checks, heal, force, flap=None, now=None)` used consistently in Tasks 3/6; `CheckResult(..., grade=...)` field added Task 1 and used everywhere; `read_config()` 3-tuple updated at its only caller (Task 6); `FlapState._c/should_heal/record_heal/recovered/is_chronic` names match across Tasks 2/3/5. ✓

## Deferred (per spec, not in this plan)

schema-board LED integration; flap thresholds in `doctor.conf`; wizard consumption of `--json`; the real fixes for the DEFERRED-named deep bugs (session-registration race, PowerDevil `login1` property).
