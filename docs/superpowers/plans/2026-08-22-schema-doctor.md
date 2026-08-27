# schema-doctor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A self-healing checker that diagnoses the schema-init logind seam every boot, auto-heals the safe faults, and names the deep ones without touching a live desktop.

**Architecture:** One stdlib-only Python file (`scripts/schema-doctor.py`) with a `Check` registry and a per-check run loop (detect → heal → verify → back-out; abort on collateral). Four v1 checks cover session identity, GPU/input ACLs, VT mediation, and login1 power probes. Runs as a late `critical=0` schema-init oneshot and as a CLI. systemd (still installed) is the oracle: live `getfacl`/`VT_GETMODE`/`busctl` reads plus systemd's shipped uaccess rules.

**Tech Stack:** Python 3 stdlib only (no `dbus`/`gi` import — shell out to `busctl`); `setfacl`/`getfacl` for ACLs; schema-init `.svc` units; self-contained python test scripts (exit 0/1) matching `tests/test_logind_*.py`.

**Spec:** `docs/superpowers/specs/2026-08-22-schema-doctor-design.md`

## Global Constraints

- **Stdlib only.** No third-party imports in `schema-doctor.py`. dbus access is via `busctl` subprocess, never the `dbus` python module.
- **Never leave the box worse than found.** Every SAFE heal snapshots first; heal-failed or collateral-breakage → back-out + report-not-healed.
- **Never block boot.** The whole run is wrapped; any uncaught exception → log + `exit 0`. The svc is `critical=0`.
- **Injectable roots for tests.** All host paths derive from `DOCTOR_ROOT` (default `""`); the active-VT source, VT-mode reader, and busctl command are overridable by env, mirroring `$SCHEMA_LOGIND_ACTIVE_VT` in `scripts/schema-logind.py`.
- **Grades:** `SAFE` auto-heals; `DEFERRED` is detect-only unless `--force <name>`.
- **Test invocation:** each `tests/test_doctor_*.py` is executable, run directly, exits 0 (all pass) / 1 (any fail). Not added to the C-only Makefile `test` target.
- **Report:** `${DOCTOR_ROOT}/var/log/schema-init/doctor-report.txt`, mode 0644.
- **Commit trailer** on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_018sNNAUbqpTn5a4mkqRhwHf
  ```

---

### Task 1: Engine core — Check contract, registry, run loop

**Files:**
- Create: `scripts/schema-doctor.py`
- Test: `tests/test_doctor_engine.py`

**Interfaces:**
- Consumes: nothing (first task).
- Produces:
  - `SAFE = "SAFE"`, `DEFERRED = "DEFERRED"`
  - `@dataclass Finding(detail: str, oracle_said: str = "", healable: bool = True)`
  - `class Check` with attrs `name: str`, `summary: str`, `grade: str` and methods `detect() -> Finding|None`, `explain(f: Finding) -> str`, `snapshot() -> Any`, `heal(f: Finding) -> None`, `verify() -> bool`, `back_out(snap: Any) -> None`. Defaults: `explain` returns `f.detail`; `verify` returns `self.detect() is None`; `snapshot`/`back_out` no-op.
  - `@dataclass CheckResult(name: str, summary: str, state: str, detail: str, oracle_said: str, action: str)` where `state ∈ {"clean","healed","reported"}`.
  - `run_checks(checks: list[Check], heal: bool, force: set[str]) -> list[CheckResult]` — the loop, including collateral-abort.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""Engine-core tests for schema-doctor: the detect/heal/verify/back-out loop."""
import os, sys, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

class Fake(sd.Check):
    def __init__(self, name, grade, broken, heal_fixes=True, breaks_on_heal=None):
        self.name, self.summary, self.grade = name, name, grade
        self._broken, self._heal_fixes, self._breaks = broken, heal_fixes, breaks_on_heal
        self.snap_taken = self.backed_out = self.healed = False
    def detect(self):
        if self._breaks is not None and self._breaks[0]:  # collateral victim
            return sd.Finding("collateral")
        return sd.Finding("broken") if self._broken else None
    def snapshot(self): self.snap_taken = True; return "S"
    def heal(self, f):
        self.healed = True
        if self._heal_fixes: self._broken = False
        if self._breaks is not None: self._breaks[0] = True  # trip a sibling
    def back_out(self, snap): self.backed_out = True; assert snap == "S"

# clean check → state clean, no heal
r = sd.run_checks([Fake("a", sd.SAFE, broken=False)], heal=True, force=set())[0]
check("clean stays clean", r.state == "clean")

# SAFE broken, heal fixes → healed
f = Fake("b", sd.SAFE, broken=True); r = sd.run_checks([f], heal=True, force=set())[0]
check("SAFE heals", r.state == "healed" and f.snap_taken and f.healed)

# SAFE broken, heal disabled → reported, not healed
f = Fake("c", sd.SAFE, broken=True); r = sd.run_checks([f], heal=False, force=set())[0]
check("heal=False reports only", r.state == "reported" and not f.healed)

# DEFERRED broken, heal on → reported (no heal) unless forced
f = Fake("d", sd.DEFERRED, broken=True); r = sd.run_checks([f], heal=True, force=set())[0]
check("DEFERRED not auto-healed", r.state == "reported" and not f.healed)
f = Fake("d", sd.DEFERRED, broken=True); r = sd.run_checks([f], heal=True, force={"d"})[0]
check("DEFERRED heals when forced", r.state == "healed" and f.healed)

# heal does NOT fix → back_out + reported
f = Fake("e", sd.SAFE, broken=True, heal_fixes=False); r = sd.run_checks([f], heal=True, force=set())[0]
check("heal-failed backs out", r.state == "reported" and f.backed_out)

# collateral: healing 'g' breaks previously-clean 'h' → g backed out, run aborts
shared = [False]
g = Fake("g", sd.SAFE, broken=True, breaks_on_heal=shared)
h = Fake("h", sd.SAFE, broken=False, breaks_on_heal=shared)
rs = sd.run_checks([g, h], heal=True, force=set())
check("collateral backs out healer", g.backed_out)
check("collateral aborts run", any(x.name == "g" and x.state == "reported" for x in rs))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_engine.py`
Expected: FAIL — `scripts/schema-doctor.py` does not exist / `run_checks` undefined.

- [ ] **Step 3: Write minimal implementation**

Create `scripts/schema-doctor.py`:

```python
#!/usr/bin/env python3
"""schema-doctor — diagnose-and-heal the schema-init logind seam.

Runs late (after a session exists) as a critical=0 oneshot and as a CLI.
Each invariant is a Check; the run loop snapshots before every SAFE heal and
backs out on failure or collateral so the box is never left worse. systemd,
still installed, is the oracle. Stdlib only — dbus is reached via busctl.
"""
import os
from dataclasses import dataclass, field
from typing import Any, Optional

SAFE, DEFERRED = "SAFE", "DEFERRED"
ROOT = os.environ.get("DOCTOR_ROOT", "")


@dataclass
class Finding:
    detail: str
    oracle_said: str = ""
    healable: bool = True


@dataclass
class CheckResult:
    name: str
    summary: str
    state: str          # clean | healed | reported
    detail: str = ""
    oracle_said: str = ""
    action: str = ""


class Check:
    name = "check"
    summary = ""
    grade = SAFE

    def detect(self) -> Optional[Finding]:
        raise NotImplementedError

    def explain(self, f: Finding) -> str:
        return f.detail

    def snapshot(self) -> Any:
        return None

    def heal(self, f: Finding) -> None:
        pass

    def verify(self) -> bool:
        return self.detect() is None

    def back_out(self, snap: Any) -> None:
        pass


def run_checks(checks, heal, force):
    results = {}
    order = []
    clean = []          # checks that detected healthy — watched for collateral
    aborted = False
    for c in checks:
        order.append(c.name)
        if aborted:
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          "not run — earlier heal was rolled back",
                                          "", "run aborted")
            continue
        f = c.detect()
        if f is None:
            results[c.name] = CheckResult(c.name, c.summary, "clean")
            clean.append(c)
            continue
        will_heal = heal and f.healable and (c.grade == SAFE or c.name in force)
        if not will_heal:
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          c.explain(f), f.oracle_said,
                                          "left as found (deferred)" if c.grade == DEFERRED
                                          else "detect-only")
            continue
        snap = c.snapshot()
        c.heal(f)
        # collateral: did any previously-clean check just break?
        broke = next((x for x in clean if x.detect() is not None), None)
        if broke is not None:
            c.back_out(snap)
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          c.explain(f), f.oracle_said,
                                          f"rolled back — heal broke {broke.name}")
            aborted = True
            continue
        if c.verify():
            results[c.name] = CheckResult(c.name, c.summary, "healed",
                                          c.explain(f), f.oracle_said, "healed")
            clean.append(c)
        else:
            c.back_out(snap)
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          c.explain(f), f.oracle_said,
                                          "heal did not resolve — rolled back")
    return [results[n] for n in order]
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_engine.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
chmod +x scripts/schema-doctor.py tests/test_doctor_engine.py
git add scripts/schema-doctor.py tests/test_doctor_engine.py
git commit -m "feat(doctor): engine core — Check contract + run loop with back-out"
```

---

### Task 2: Report writer, config, CLI, session-up gate

**Files:**
- Modify: `scripts/schema-doctor.py`
- Test: `tests/test_doctor_cli.py`

**Interfaces:**
- Consumes: `run_checks`, `CheckResult`, `ROOT` (Task 1).
- Produces:
  - `read_config() -> tuple[bool, set[str]]` → `(heal_enabled, disabled_names)` from `${ROOT}/etc/schema-init/doctor.conf` (`heal=no`, `disable=a,b`). Missing file → `(True, set())`.
  - `render_report(results: list[CheckResult]) -> str` — human text, one block per check.
  - `render_json(results) -> str` — `json.dumps` of a list of dicts.
  - `write_report(text: str) -> str` — writes `${ROOT}/var/log/schema-init/doctor-report.txt` (0644), returns path.
  - `wait_for_session(timeout: float) -> bool` — polls `${ROOT}/run/systemd/sessions/` for any non-`31` session file; returns True on found / False on timeout.
  - `main(argv) -> int` — parses `--check/--heal/--explain/--dry-run/--force/--json`, applies config, calls `run_checks`, writes report, prints. Always returns 0.
  - `REGISTRY: list[Check]` — empty list for now; later tasks append.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""CLI/config/report tests for schema-doctor."""
import os, sys, json, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def load(root):
    os.environ["DOCTOR_ROOT"] = root
    spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}")

root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "etc/schema-init"))
sd = load(root)

# config: missing file → heal on, nothing disabled
heal, dis = sd.read_config(); check("default config heals", heal is True and dis == set())

# config: heal=no + disable list
with open(os.path.join(root, "etc/schema-init/doctor.conf"), "w") as fh:
    fh.write("heal=no\ndisable=foo, bar\n")
heal, dis = sd.read_config(); check("config parsed", heal is False and dis == {"foo", "bar"})

# report writer creates a 0644 file
res = [sd.CheckResult("x", "x summary", "healed", "was broken", "systemd: rw", "healed")]
p = sd.write_report(sd.render_report(res))
check("report written 0644", os.path.exists(p) and (os.stat(p).st_mode & 0o777) == 0o644)
check("report names the check", "x summary" in open(p).read())

# json render is valid and carries state
j = json.loads(sd.render_json(res)); check("json ok", j[0]["state"] == "healed")

# session gate: a real session file (id != 31) satisfies it fast
os.makedirs(os.path.join(root, "run/systemd/sessions"))
open(os.path.join(root, "run/systemd/sessions", "1"), "w").close()
check("session gate sees session 1", sd.wait_for_session(1.0) is True)

# main always returns 0 even with an exploding check in the registry
class Boom(sd.Check):
    name = "boom"; summary = "boom"; grade = sd.SAFE
    def detect(self): raise RuntimeError("kaboom")
sd.REGISTRY[:] = [Boom()]
check("main survives an exploding check", sd.main(["--check"]) == 0)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_cli.py`
Expected: FAIL — `read_config` / `render_report` / `main` undefined.

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/schema-doctor.py` (add `import json, sys, time` to the top imports):

```python
REGISTRY: list = []


def read_config():
    heal = True
    disabled = set()
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
    except FileNotFoundError:
        pass
    return heal, disabled


def render_report(results):
    lines = ["=== schema-doctor report ===", ""]
    for r in results:
        lines.append(f"[{r.state.upper()}] {r.name} — {r.summary}")
        if r.detail:
            lines.append(f"    found:  {r.detail}")
        if r.oracle_said:
            lines.append(f"    systemd would: {r.oracle_said}")
        if r.action:
            lines.append(f"    action: {r.action}")
        lines.append("")
    return "\n".join(lines)


def render_json(results):
    return json.dumps([r.__dict__ for r in results], indent=2)


def write_report(text):
    d = os.path.join(ROOT, "var/log/schema-init")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "doctor-report.txt")
    with open(p, "w") as fh:
        fh.write(text)
    os.chmod(p, 0o644)
    return p


def wait_for_session(timeout):
    d = os.path.join(ROOT, "run/systemd/sessions")
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if any(n != "31" and not n.endswith(".ref") for n in os.listdir(d)):
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.25)
    return False


def _safe_detect_all(checks, heal, force):
    try:
        return run_checks(checks, heal, force)
    except Exception as e:                       # never propagate — never block boot
        return [CheckResult("schema-doctor", "internal", "reported",
                            f"doctor aborted: {e}", "", "logged, exit 0")]


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="schema-doctor")
    ap.add_argument("--check", action="store_true", help="detect + report, no heal")
    ap.add_argument("--heal", action="store_true", help="detect + heal + re-check")
    ap.add_argument("--explain", metavar="NAME", help="plain-language why for one check")
    ap.add_argument("--dry-run", action="store_true", help="show heals, change nothing")
    ap.add_argument("--force", action="append", default=[], metavar="NAME",
                    help="run a DEFERRED check's heal anyway")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--wait", type=float, default=0.0, help="wait N s for a session first")
    args = ap.parse_args(argv)

    cfg_heal, disabled = read_config()
    checks = [c for c in REGISTRY if c.name not in disabled]

    if args.explain:
        for c in checks:
            if c.name == args.explain:
                f = None
                try:
                    f = c.detect()
                except Exception as e:
                    print(f"{c.name}: detect failed: {e}"); return 0
                print(c.explain(f) if f else f"{c.name}: healthy")
                return 0
        print(f"unknown check: {args.explain}"); return 0

    if args.wait:
        wait_for_session(args.wait)

    heal = (args.heal or (not args.check and not args.dry_run)) and cfg_heal and not args.dry_run
    results = _safe_detect_all(checks, heal, set(args.force))

    out = render_json(results) if args.json else render_report(results)
    write_report(render_report(results))
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_cli.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
chmod +x tests/test_doctor_cli.py
git add scripts/schema-doctor.py tests/test_doctor_cli.py
git commit -m "feat(doctor): config, report writer, CLI, session-up gate"
```

---

### Task 3: `card-input-acl` check (SAFE) — the poster child

**Files:**
- Modify: `scripts/schema-doctor.py`
- Test: `tests/test_doctor_acl.py`

**Interfaces:**
- Consumes: `Check`, `Finding`, `SAFE`, `ROOT`, `REGISTRY` (Tasks 1–2).
- Produces: `class CardInputAcl(Check)` with `name = "card-input-acl"`, `grade = SAFE`. Helpers: `active_uid() -> int|None` (reads the non-31 session file's `UID=` under `${ROOT}/run/systemd/sessions`), `acl_nodes() -> list[str]` (globs `${ROOT}/dev/dri/card*`, `renderD*`, `/dev/input/event*`), `_getfacl(path) -> str`, `_has_rw(path, uid) -> bool`, `_setfacl_rw(path, uid)`, `_clearfacl(path, uid)`. Appended to `REGISTRY`.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""card-input-acl tests — real setfacl/getfacl on temp files, no root needed."""
import os, sys, tempfile, subprocess, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
# fake a session so active_uid() resolves to *this* user
os.makedirs(os.path.join(root, "run/systemd/sessions"))
uid = os.getuid()
with open(os.path.join(root, "run/systemd/sessions", "1"), "w") as fh:
    fh.write(f"UID={uid}\nVTNR=1\n")
os.makedirs(os.path.join(root, "dev/dri"))
node = os.path.join(root, "dev/dri/card0")
open(node, "w").close()   # stand-in device node — ACLs apply to any file

spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.CardInputAcl()
# no user ACL yet → detect finds it broken
subprocess.run(["setfacl", "-b", node], check=True)
f = c.detect(); check("detect flags missing ACL", f is not None)

# heal → user gets rw, verify passes
snap = c.snapshot(); c.heal(f); check("heal applies rw", c.verify() is True)
out = subprocess.run(["getfacl", "-p", node], capture_output=True, text=True).stdout
check("getfacl shows user rw", f"user:{uid}:rw" in out)

# back_out restores the pre-heal ACL (no user entry)
c.back_out(snap)
out = subprocess.run(["getfacl", "-p", node], capture_output=True, text=True).stdout
check("back_out removes user rw", f"user:{uid}:rw" not in out)

# idempotent: heal twice, second is a no-op that still verifies
f = c.detect(); c.heal(f); c.heal(c.detect() or sd.Finding("x")); check("idempotent", c.verify() is True)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_acl.py`
Expected: FAIL — `sd.CardInputAcl` undefined.

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/schema-doctor.py` (`import glob, subprocess` at top):

```python
def active_uid():
    d = os.path.join(ROOT, "run/systemd/sessions")
    try:
        names = [n for n in os.listdir(d) if n != "31" and not n.endswith(".ref")]
    except FileNotFoundError:
        return None
    for n in sorted(names):
        try:
            for line in open(os.path.join(d, n)):
                if line.startswith("UID="):
                    return int(line.strip()[4:])
        except (OSError, ValueError):
            continue
    return None


class CardInputAcl(Check):
    name = "card-input-acl"
    summary = "the logged-in user can open the GPU and input devices"
    grade = SAFE

    def _nodes(self):
        pats = ["dev/dri/card*", "dev/dri/renderD*", "dev/input/event*"]
        out = []
        for p in pats:
            out += glob.glob(os.path.join(ROOT, p))
        return sorted(out)

    def _getfacl(self, path):
        return subprocess.run(["getfacl", "-pn", path],
                              capture_output=True, text=True).stdout

    def _has_rw(self, path, uid):
        return f"user:{uid}:rw" in self._getfacl(path)

    def detect(self):
        uid = active_uid()
        if uid is None or uid == 0:
            return None
        missing = [n for n in self._nodes() if not self._has_rw(n, uid)]
        if not missing:
            return None
        return Finding(
            detail=f"uid {uid} lacks rw on: {', '.join(os.path.basename(m) for m in missing)}",
            oracle_said="systemd uaccess grants the active-seat user rw on these",
            healable=True)

    def snapshot(self):
        return {n: self._getfacl(n) for n in self._nodes()}

    def heal(self, f):
        uid = active_uid()
        for n in self._nodes():
            if not self._has_rw(n, uid):
                subprocess.run(["setfacl", "-m", f"u:{uid}:rw", n], check=False)

    def back_out(self, snap):
        for n, acl in snap.items():
            subprocess.run(["setfacl", "--set-file=-", n],
                           input=acl, text=True, check=False)


REGISTRY.append(CardInputAcl())
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_acl.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
chmod +x tests/test_doctor_acl.py
git add scripts/schema-doctor.py tests/test_doctor_acl.py
git commit -m "feat(doctor): card-input-acl check — auto-heal uaccess ACLs"
```

---

### Task 4: `session-single` check (DEFERRED, detect-only)

**Files:**
- Modify: `scripts/schema-doctor.py`
- Test: `tests/test_doctor_session.py`

**Interfaces:**
- Consumes: `Check`, `Finding`, `DEFERRED`, `ROOT`, `REGISTRY`.
- Produces: `class SessionSingle(Check)` with `name = "session-single"`, `grade = DEFERRED`. Reads the active VT from `${ROOT}` + env `SCHEMA_DOCTOR_ACTIVE_VT` (default `sys/class/tty/tty0/active`, value like `tty1`). Broken when the only/active session is the synthesised `LEGACY_ID=31` with `VTNR=0`, or when no session carries the live VTNR. Appended to `REGISTRY`.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""session-single tests — orphan #31 detection against a fake session tree."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def fresh(sessions, active_vt="tty1"):
    root = tempfile.mkdtemp()
    sd_dir = os.path.join(root, "run/systemd/sessions"); os.makedirs(sd_dir)
    tty = os.path.join(root, "sys/class/tty/tty0"); os.makedirs(tty)
    with open(os.path.join(tty, "active"), "w") as fh: fh.write(active_vt + "\n")
    for sid, body in sessions.items():
        with open(os.path.join(sd_dir, sid), "w") as fh: fh.write(body)
    os.environ["DOCTOR_ROOT"] = root
    spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# healthy: real session 1 on VTNR 1, no orphan
sd = fresh({"1": "UID=1000\nVTNR=1\nLEADER=1106\n"})
check("clean session passes", sd.SessionSingle().detect() is None)

# broken: only the synthesised legacy #31 on VTNR 0
sd = fresh({"31": "LEGACY_ID=31\nUID=1000\nVTNR=0\nLEADER=\n"})
f = sd.SessionSingle().detect()
check("orphan #31 flagged", f is not None and "31" in f.detail)

# broken: #31 coexists with a real session but active VT has no matching session
sd = fresh({"31": "UID=1000\nVTNR=0\n", "1": "UID=1000\nVTNR=1\n"}, active_vt="tty2")
check("active VT unbacked flagged", sd.SessionSingle().detect() is not None)

# grade is DEFERRED (never auto-heals)
check("grade DEFERRED", sd.SessionSingle().grade == sd.DEFERRED)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_session.py`
Expected: FAIL — `sd.SessionSingle` undefined.

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/schema-doctor.py`:

```python
def _active_vtnr():
    rel = os.environ.get("SCHEMA_DOCTOR_ACTIVE_VT", "sys/class/tty/tty0/active")
    try:
        val = open(os.path.join(ROOT, rel)).read().strip()   # e.g. "tty1"
        return int(val[3:]) if val.startswith("tty") else None
    except (OSError, ValueError):
        return None


def _sessions():
    d = os.path.join(ROOT, "run/systemd/sessions")
    out = {}
    try:
        names = [n for n in os.listdir(d) if not n.endswith(".ref")]
    except FileNotFoundError:
        return out
    for n in names:
        kv = {}
        try:
            for line in open(os.path.join(d, n)):
                k, _, v = line.strip().partition("=")
                kv[k] = v
        except OSError:
            continue
        out[n] = kv
    return out


class SessionSingle(Check):
    name = "session-single"
    summary = "one real desktop session, no orphaned placeholder"
    grade = DEFERRED

    def detect(self):
        sess = _sessions()
        if not sess:
            return None
        vtnr = _active_vtnr()
        orphan = "31" in sess and sess["31"].get("VTNR", "0") == "0"
        backed = any(s.get("VTNR") == str(vtnr) for s in sess.values()) if vtnr else False
        active_is_orphan = orphan and (len(sess) == 1 or not backed)
        if active_is_orphan:
            return Finding(
                detail="the active session is the synthesised placeholder #31 "
                       "(VTNR=0) — session registration lost the boot-time race",
                oracle_said=f"a real session should own the live VT (tty{vtnr})",
                healable=False)
        if vtnr and not backed:
            return Finding(
                detail=f"no registered session owns the active VT (tty{vtnr})",
                oracle_said=f"the logged-in session should carry VTNR={vtnr}",
                healable=False)
        return None


REGISTRY.append(SessionSingle())
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_session.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
chmod +x tests/test_doctor_session.py
git add scripts/schema-doctor.py tests/test_doctor_session.py
git commit -m "feat(doctor): session-single check — name the orphan-#31 race"
```

---

### Task 5: `login1-power` check (DEFERRED, detect-only) via busctl

**Files:**
- Modify: `scripts/schema-doctor.py`
- Test: `tests/test_doctor_power.py`

**Interfaces:**
- Consumes: `Check`, `Finding`, `DEFERRED`, `REGISTRY`.
- Produces: `class Login1Power(Check)` with `name = "login1-power"`, `grade = DEFERRED`. Probes the login1 Manager methods PowerDevil calls on load: `CanPowerOff`, `CanReboot`, `CanSuspend`, `CanHibernate`, `ListInhibitors`. The probe command is an injectable callable `self.probe(method) -> tuple[int, str]` (returncode, output); production shells out to `busctl call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager <method>`. Overridable via `SCHEMA_DOCTOR_BUSCTL` env (a script path) for tests. Appended to `REGISTRY`.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""login1-power tests — probe injected, no live bus touched."""
import os, sys, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.Login1Power()

# all methods answer → healthy
c.probe = lambda m: (0, 's "yes"')
check("all-answered passes", c.detect() is None)

# one method errors → flagged, and names the failing method
def one_fails(m): return (1, "Unknown method") if m == "ListInhibitors" else (0, 's "yes"')
c.probe = one_fails
f = c.detect()
check("failing method flagged", f is not None and "ListInhibitors" in f.detail)

# grade DEFERRED, not auto-healed
check("grade DEFERRED", c.grade == sd.DEFERRED)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_power.py`
Expected: FAIL — `sd.Login1Power` undefined.

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/schema-doctor.py`:

```python
class Login1Power(Check):
    name = "login1-power"
    summary = "the power controls (PowerDevil) can read login1"
    grade = DEFERRED

    METHODS = ["CanPowerOff", "CanReboot", "CanSuspend", "CanHibernate", "ListInhibitors"]

    def probe(self, method):
        cmd = os.environ.get("SCHEMA_DOCTOR_BUSCTL", "busctl")
        try:
            r = subprocess.run(
                [cmd, "call", "org.freedesktop.login1", "/org/freedesktop/login1",
                 "org.freedesktop.login1.Manager", method],
                capture_output=True, text=True, timeout=5)
            return r.returncode, (r.stdout + r.stderr).strip()
        except Exception as e:
            return 1, str(e)

    def detect(self):
        failed = [m for m in self.METHODS if self.probe(m)[0] != 0]
        if not failed:
            return None
        return Finding(
            detail=f"login1 did not answer: {', '.join(failed)} "
                   "(this is why PowerDevil says settings could not be loaded)",
            oracle_said="systemd-logind answers all of these on the Manager interface",
            healable=False)


REGISTRY.append(Login1Power())
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_power.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
chmod +x tests/test_doctor_power.py
git add scripts/schema-doctor.py tests/test_doctor_power.py
git commit -m "feat(doctor): login1-power check — fingerprint the PowerDevil probe"
```

---

### Task 6: `vt-mediation` check — detect (VT_GETMODE), heal via re-arm hook

**Files:**
- Modify: `scripts/schema-doctor.py`
- Test: `tests/test_doctor_vt.py`

**Interfaces:**
- Consumes: `Check`, `Finding`, `SAFE`, `REGISTRY`, `Login1Power`-style injectable probe pattern.
- Produces: `class VtMediation(Check)` with `name = "vt-mediation"`, `grade = SAFE`. `vt_mode() -> str` returns `"process"` / `"auto"` / `"unknown"`; production does `VT_GETMODE` ioctl on `/dev/tty0`, tests override via env `SCHEMA_DOCTOR_VT_MODE`. `heal()` calls the schema-logind re-arm hook via `self.rearm()` (injectable; production shells `busctl call ... org.schema.logind1.Manager RearmVtMediation`). If the hook is unreachable, heal is a no-op and `verify()` stays False → run loop reports-not-healed (graceful degrade to detect-only). Appended to `REGISTRY`.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""vt-mediation tests — VT mode injected via env, re-arm hook injected."""
import os, sys, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def load(mode):
    os.environ["SCHEMA_DOCTOR_VT_MODE"] = mode
    spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# VT_PROCESS → mediated → healthy
sd = load("process"); check("mediated passes", sd.VtMediation().detect() is None)

# VT_AUTO → not mediated → broken (the frozen-mouse path)
sd = load("auto"); c = sd.VtMediation()
f = c.detect(); check("auto flagged", f is not None)

# heal calls re-arm; if the hook flips the mode to process, verify passes
armed = {"n": 0}
def fake_rearm():
    armed["n"] += 1; os.environ["SCHEMA_DOCTOR_VT_MODE"] = "process"
c.rearm = fake_rearm
c.heal(f); check("heal calls re-arm", armed["n"] == 1 and c.verify() is True)

# hook unreachable → heal no-ops, verify stays False (degrades to report)
sd = load("auto"); c = sd.VtMediation(); c.rearm = lambda: None
c.heal(c.detect()); check("unreachable hook stays broken", c.verify() is False)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_vt.py`
Expected: FAIL — `sd.VtMediation` undefined.

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/schema-doctor.py` (`import fcntl, struct` at top):

```python
# linux/vt.h
VT_GETMODE = 0x5601
VT_AUTO, VT_PROCESS = 0x00, 0x01


class VtMediation(Check):
    name = "vt-mediation"
    summary = "Ctrl+Alt+F-keys are mediated (VT switching won't freeze the screen)"
    grade = SAFE

    def vt_mode(self):
        env = os.environ.get("SCHEMA_DOCTOR_VT_MODE")
        if env:
            return env
        try:
            fd = os.open(os.path.join(ROOT, "dev/tty0"), os.O_RDONLY | os.O_NOCTTY)
        except OSError:
            return "unknown"
        try:
            buf = fcntl.ioctl(fd, VT_GETMODE, struct.pack("bbhhh", 0, 0, 0, 0, 0))
            mode = struct.unpack("bbhhh", buf)[0]
            return "process" if mode == VT_PROCESS else "auto"
        except OSError:
            return "unknown"
        finally:
            os.close(fd)

    def rearm(self):
        cmd = os.environ.get("SCHEMA_DOCTOR_BUSCTL", "busctl")
        subprocess.run(
            [cmd, "call", "org.freedesktop.login1", "/org/freedesktop/login1",
             "org.schema.logind1.Manager", "RearmVtMediation"],
            capture_output=True, text=True, check=False, timeout=5)

    def detect(self):
        m = self.vt_mode()
        if m in ("process", "unknown"):     # unknown = can't prove broken; don't cry wolf
            return None
        return Finding(
            detail="the active VT is in kernel-native switching (VT_AUTO), not "
                   "mediated by schema-logind — switching consoles can freeze the screen",
            oracle_said="logind puts the session's VT in VT_PROCESS mode",
            healable=True)

    def heal(self, f):
        try:
            self.rearm()
        except Exception:
            pass

    def verify(self):
        return self.vt_mode() == "process"


REGISTRY.append(VtMediation())
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_vt.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
chmod +x tests/test_doctor_vt.py
git add scripts/schema-doctor.py tests/test_doctor_vt.py
git commit -m "feat(doctor): vt-mediation check — detect VT_AUTO, heal via re-arm hook"
```

---

### Task 7: schema-logind `RearmVtMediation` dbus hook

**Files:**
- Modify: `scripts/schema-logind.py` (add a method on the Manager object; the class registering `org.freedesktop.login1.Manager` around line 1682–1790)
- Test: `tests/test_doctor_rearm.py`

**Interfaces:**
- Consumes: schema-logind's existing Manager dbus object and its `_setup_vt_mediation` (line ~889) and active-session lookup.
- Produces: a dbus method `RearmVtMediation` on interface `org.schema.logind1.Manager` (same object path `/org/freedesktop/login1`) that finds the active session and calls its `_setup_vt_mediation()`, returning `""` on success or an error string. Idempotent (the setup is guarded `if self.vt_fd is not None: return`).

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""RearmVtMediation hook test — private bus, drive schema-logind, assert the
method exists and returns cleanly. Mirrors tests/test_logind_vt.py setup."""
import os, sys, subprocess, time, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGIND = os.path.join(REPO, "scripts", "schema-logind.py")

# reuse the private-bus harness style from test_logind_vt.py
import dbus
from dbus.mainloop.glib import DBusGMainLoop
DBusGMainLoop(set_as_default=True)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# start a private session bus
bus_addr = subprocess.check_output(["dbus-daemon", "--session", "--print-address",
                                    "--fork", "--print-pid"], text=True).strip().splitlines()
os.environ["DBUS_SESSION_BUS_ADDRESS"] = bus_addr[0]
active_vt = tempfile.NamedTemporaryFile("w", delete=False); active_vt.write("tty1\n"); active_vt.close()
env = dict(os.environ, SCHEMA_LOGIND_ACTIVE_VT=active_vt.name,
           SCHEMA_LOGIND_BUS="session")
p = subprocess.Popen([sys.executable, LOGIND], env=env)
time.sleep(2.0)
try:
    bus = dbus.SessionBus()
    obj = bus.get_object("org.freedesktop.login1", "/org/freedesktop/login1")
    mgr = dbus.Interface(obj, "org.schema.logind1.Manager")
    err = str(mgr.RearmVtMediation())
    check("RearmVtMediation callable", True)
    check("returns no error", err == "")
except Exception as e:
    check("RearmVtMediation callable", False); print("   ", e)
finally:
    p.terminate()

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

> Note: if schema-logind has no `SCHEMA_LOGIND_BUS`/private-bus switch, follow the exact harness already used in `tests/test_logind_vt.py` (read it first) to stand the daemon on a private bus; the assertion (method exists, returns `""`) is unchanged.

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_rearm.py`
Expected: FAIL — no `RearmVtMediation` method on the object.

- [ ] **Step 3: Write minimal implementation**

In `scripts/schema-logind.py`, on the class that owns the Manager object (the one with `@dbus.service.method(MANAGER_IFACE, ...)` methods like `CanPowerOff`), add:

```python
    @dbus.service.method("org.schema.logind1.Manager", in_signature="", out_signature="s")
    def RearmVtMediation(self):
        """Doctor hook: re-arm VT mediation on the active session. Idempotent —
        _setup_vt_mediation() is a no-op when vt_fd is already held."""
        try:
            sess = self._active_session()      # use the daemon's existing lookup
            if sess is None:
                return "no active session"
            sess._setup_vt_mediation()
            return ""
        except Exception as e:
            return f"rearm failed: {e}"
```

Use whatever the daemon's real active-session accessor is (grep `active` / `_active_session` / the seat's `ActiveSession`); if none exists, pick the session whose `vtnr` matches the current active VT. Keep the method body tiny.

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_rearm.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
chmod +x tests/test_doctor_rearm.py
git add scripts/schema-logind.py tests/test_doctor_rearm.py
git commit -m "feat(logind): RearmVtMediation hook for schema-doctor"
```

---

### Task 8: Service unit, install wiring, ACL integration vmtest

**Files:**
- Create: `services/schema-doctor.svc`
- Create: `tests/livetest/doctor-acl-vmtest.sh`
- Modify: `Makefile` (install `schema-doctor.py` → `/usr/local/bin/schema-doctor`, following the `schema-logind.py` install rule)
- Modify: `distros/fedora-installer/rail/scripts/schema-sysprep.sh` (nothing) — instead ensure the svc ships; check `distros/fedora-installer/schema.ks` installs `schema-doctor.svc` + the script alongside `schema-logind`.

**Interfaces:**
- Consumes: the finished `scripts/schema-doctor.py` (all tasks).
- Produces: a bootable `schema-doctor.svc` and a green ACL integration test.

- [ ] **Step 1: Write the failing integration test**

Create `tests/livetest/doctor-acl-vmtest.sh`:

```sh
#!/bin/sh
# schema-doctor ACL heal, on a real device node under a real session tree.
# Strips the uaccess ACL off a card node, runs schema-doctor --heal in a
# DOCTOR_ROOT sandbox, asserts the active uid regains rw and back-out restores.
set -eu
REPO="${REPO:-$HOME/projects/schema-init}"
DOC="$REPO/scripts/schema-doctor.py"
W="$(mktemp -d /var/tmp/schema-doctor.XXXX)"
trap 'rm -rf "$W"' EXIT
UID_T="$(id -u)"

mkdir -p "$W/run/systemd/sessions" "$W/dev/dri"
printf 'UID=%s\nVTNR=1\n' "$UID_T" > "$W/run/systemd/sessions/1"
: > "$W/dev/dri/card0"
setfacl -b "$W/dev/dri/card0"

DOCTOR_ROOT="$W" python3 "$DOC" --heal >/dev/null
if getfacl -pn "$W/dev/dri/card0" | grep -q "user:$UID_T:rw"; then
  echo ">> RESULT: PASS  (doctor restored uaccess rw)"
else
  echo ">> RESULT: FAIL  (ACL not applied; workdir $W)"; trap - EXIT; exit 1
fi
```

- [ ] **Step 2: Run it to verify it fails**

Run: `sh tests/livetest/doctor-acl-vmtest.sh`
Expected: FAIL until the script is executable/wired (and PASS once Task 3 code is present — this is the end-to-end confirmation, not a new unit).

- [ ] **Step 3: Create the service unit and install wiring**

Create `services/schema-doctor.svc`:

```
name=schema-doctor
exec=/usr/local/bin/schema-doctor --heal --wait 30
dep=schema-logind
oneshot=1
needs_root=1
critical=0
```

In `Makefile`, find the rule that installs `schema-logind.py` and add the twin (same `install -m 0755` target dir, `/usr/local/bin/schema-doctor`). In `distros/fedora-installer/schema.ks`, wherever `schema-logind.svc` / `schema-logind.py` are copied into the image, add `schema-doctor.svc` and `schema-doctor.py` next to them. (Grep both files for `schema-logind` and mirror every occurrence.)

- [ ] **Step 4: Verify the wiring**

Run:
```bash
grep -n schema-doctor Makefile distros/fedora-installer/schema.ks services/schema-doctor.svc
sh tests/livetest/doctor-acl-vmtest.sh
```
Expected: `schema-doctor` appears in Makefile install + ks + the svc file; the vmtest prints `RESULT: PASS`.

- [ ] **Step 5: Run the full doctor test sweep + commit**

```bash
for t in engine cli acl session power vt; do python3 tests/test_doctor_$t.py || exit 1; done
chmod +x services/schema-doctor.svc tests/livetest/doctor-acl-vmtest.sh
git add services/schema-doctor.svc tests/livetest/doctor-acl-vmtest.sh Makefile distros/fedora-installer/schema.ks
git commit -m "feat(doctor): ship schema-doctor.svc + install wiring + ACL vmtest"
```

---

## Self-Review

**Spec coverage:**
- Architecture (single stdlib file, svc + CLI, late run, always exit 0) → Tasks 1, 2, 8. ✔
- Check interface (detect/explain/snapshot/heal/verify/back_out, SAFE/DEFERRED) → Task 1. ✔
- Run loop with per-check back-out + collateral abort → Task 1. ✔
- v1 check `card-input-acl` (SAFE) → Task 3. ✔
- v1 check `session-single` (DEFERRED) → Task 4. ✔
- v1 check `login1-power` (DEFERRED) → Task 5. ✔
- v1 check `vt-mediation` (SAFE + degrade) → Task 6. ✔
- systemd oracle: tier-1 live reads (getfacl/VT_GETMODE/busctl) → Tasks 3,5,6; tier-2 shipped uaccess rules referenced in `oracle_said` text (the grant rule is systemd's uaccess) → Task 3. ✔ *(Note: the plan encodes the uaccess expectation as "active-seat user gets rw" rather than re-parsing `73-seat-late.rules` at runtime; this matches systemd's behavior and avoids a rules parser in v1. Acceptable narrowing of spec tier-2.)*
- Config `doctor.conf` (heal=no, disable=) → Task 2. ✔
- Report (0644 text + --json) → Task 2. ✔
- Testing: per-check units → Tasks 1–6; ACL integration vmtest → Task 8; on-metal acceptance is deferred (spec says so; eli reinstall). ✔
- Dependency: schema-logind re-arm hook → Task 7. ✔

**Placeholder scan:** No TBD/TODO. Every code step carries runnable code. Task 7 and Task 8 Step 3 contain "grep the real accessor / mirror every occurrence" instructions rather than a literal line number — justified because those anchor points (schema-logind's active-session accessor; the exact Makefile/ks install lines) must be read live; the *code to add* is fully specified.

**Type consistency:** `Finding(detail, oracle_said, healable)`, `CheckResult(name, summary, state, detail, oracle_said, action)`, `run_checks(checks, heal, force)`, `Check.{detect,explain,snapshot,heal,verify,back_out}`, `active_uid`, `_active_vtnr`, `_sessions`, `REGISTRY`, `vt_mode`/`rearm`/`probe` injectables — all names used consistently across Tasks 1–8.
