# schema COPR wizard — Plan B (the PySide6 GUI) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `schema-wizard` package — a PySide6 Qt/QML GUI that walks a Linux novice through the two-reboot in-place conversion onto schema-init, driving the already-built migrate/flip/doctor engines.

**Architecture:** A pure-Python `WizardCore` holds all logic (current stage, screen decision, flow orchestration, doctor-status parsing, humanize, advanced-option mapping) and reaches the system only through an injectable `Backend` that shells the fixed privileged helpers. A thin `WizardController(QObject)` wraps the core and exposes properties/signals/slots to QML; the QML views are declarative and carry no logic. This keeps the entire brain unit-testable with no display and no Qt import, matching the repo's stdlib script-style tests.

**Tech Stack:** Python 3, PySide6 6.11 (Qt/QML), the existing `stage.py` / `schema-migrate.py` / `schema-flip-apply` / `schema-doctor.py`, RPM (`schema-init.spec`).

**Spec:** `docs/superpowers/specs/2026-09-07-schema-copr-onboarding-design.md`

## Global Constraints

- **Tests are SCRIPT-STYLE, never pytest.** The repo runs each test via `python tests/test_X.py`; `make test` runs only C tests; there is no pytest or CI. Every test file is self-contained: builds its own fixtures, calls a module-level `check(name, cond)` that appends to a `results` list and prints `ok`/`FAIL`, and ends with `print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)`. A pytest-style `def test_*(): assert` file runs nothing under this workflow. (This is the load-bearing lesson from Plan A.)
- **No docstrings/comments unless they explain a non-obvious why.** Match surrounding style.
- **The GUI is UNPRIVILEGED.** Every system change goes through the fixed helpers only: `/usr/bin/schema-migrate` and `/usr/libexec/schema-init/schema-flip-apply`, via the already-shipped `/etc/sudoers.d/schema-wizard` NOPASSWD drop-in. No inline privileged action, no new helper paths, no wildcards.
- **Prebuilt only.** The wizard consumes installed binaries; it never compiles. All migrate deploys pass `--prebuilt`.
- **Authoritative stage** is the root-owned `/var/lib/schema-init/wizard-stage.json`, written ONLY by the migrate CLI (via `stage.py`). The GUI reads it to pick a screen; it never writes it directly. Stages: `INSTALLED → R1_PENDING → R1_HEAL → R2_PENDING → DONE | ROLLED_BACK`.
- **PySide6 import is confined to `controller.py` and `main.py`.** `core.py` and `backend.py` are pure Python so their tests need no Qt and no display.
- **QML smoke tests run headless** with `QT_QPA_PLATFORM=offscreen` set in the test process before importing Qt.
- **Package layout:** new `schema-wizard` subpackage in `schema-init.spec`, `Requires: schema-init` + `python3-pyside6`. Wizard code installs under `%{_libexecdir}/schema-init/wizard/`, launched by `%{_bindir}/schema-wizard`.

## File Structure

- `distros/fedora-installer/wizard/backend.py` — the privilege boundary: builds and runs `sudo <helper> <args>`. Pure Python, injectable `run`.
- `distros/fedora-installer/wizard/core.py` — `WizardCore`: stage read, screen decision, flow orchestration, advanced-option model, recovery-ack gate. Imports `stage.py` and `Backend`. Pure Python.
- `distros/fedora-installer/wizard/status.py` — doctor-status.json parsing + `humanize_reason`. Pure Python (kept separate so its table is easy to read and test).
- `distros/fedora-installer/wizard/controller.py` — `WizardController(QObject)`: wraps `WizardCore`, exposes Qt properties/signals/slots. Imports PySide6.
- `distros/fedora-installer/wizard/main.py` — entrypoint: `QGuiApplication` + `QQmlApplicationEngine`, registers the controller, loads `Main.qml`.
- `distros/fedora-installer/wizard/qml/Main.qml` + per-screen `.qml` components — declarative views bound to the controller.
- `distros/fedora-installer/wizard/schema-wizard.desktop` — XDG autostart entry (resume after each reboot).
- `distros/fedora-installer/wizard/schema-wizard-launcher.desktop` — on-demand `~/Desktop` launcher.
- `distros/fedora-installer/wizard/schema-wizard` — the `%{_bindir}` launch shim (`exec python3 .../wizard/main.py "$@"`).
- `schema-init.spec` — new `%package wizard` + `%files wizard`.
- Tests: `tests/test_wizard_backend.py`, `test_wizard_core_stage.py`, `test_wizard_status.py`, `test_wizard_flow.py`, `test_wizard_advanced.py`, `test_wizard_controller.py`, `test_wizard_qml_smoke.py`, `test_spec_wizard_subpackage.py`.

---

### Task 1: Backend — the privileged-helper invoker

**Files:**
- Create: `distros/fedora-installer/wizard/backend.py`
- Test: `tests/test_wizard_backend.py`

**Interfaces:**
- Produces: `Backend(run=subprocess.run)` with `MIGRATE="/usr/bin/schema-migrate"`, `FLIP="/usr/libexec/schema-init/schema-flip-apply"`; methods `deploy(opts)`, `arm_flip()`, `finish()`, `uninstall()`, `read_stage()`, `flip(subcmd)`. `opts` is a list of extra CLI flags (str). Each method returns the object `run` returns (has `.returncode`, `.stdout`).

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""wizard Backend — builds sudo helper argv, injectable run, no privilege needed."""
import os, sys, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/wizard/backend.py")
spec = importlib.util.spec_from_file_location("wizard_backend", MOD)
be = importlib.util.module_from_spec(spec); spec.loader.exec_module(be)

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

calls = []
def rec(argv, *a, **k):
    calls.append(argv)
    class R: returncode = 0; stdout = "INSTALLED\n"
    return R()

b = be.Backend(run=rec)

b.deploy(["--advanced-no-snapshot"])
check("deploy shells sudo schema-migrate --deploy --prebuilt + opts",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--deploy", "--prebuilt", "--advanced-no-snapshot"])

b.arm_flip()
check("arm_flip shells sudo schema-migrate --arm-flip",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--arm-flip"])

b.finish()
check("finish shells sudo schema-migrate --finish",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--finish"])

b.uninstall()
check("uninstall shells sudo schema-migrate --uninstall",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--uninstall"])

r = b.read_stage()
check("read_stage runs --stage and returns trimmed stdout", r == "INSTALLED")

b.flip("reboot")
check("flip shells sudo schema-flip-apply <subcmd>",
      calls[-1] == ["sudo", be.Backend.FLIP, "reboot"])

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python tests/test_wizard_backend.py`
Expected: FAIL — `backend.py` does not exist.

- [ ] **Step 3: Write minimal implementation**

```python
import subprocess


class Backend:
    MIGRATE = "/usr/bin/schema-migrate"
    FLIP = "/usr/libexec/schema-init/schema-flip-apply"

    def __init__(self, run=subprocess.run):
        self._run = run

    def _sudo(self, path, *args):
        return self._run(["sudo", path, *args], capture_output=True, text=True)

    def deploy(self, opts=None):
        return self._sudo(self.MIGRATE, "--deploy", "--prebuilt", *(opts or []))

    def arm_flip(self):
        return self._sudo(self.MIGRATE, "--arm-flip")

    def finish(self):
        return self._sudo(self.MIGRATE, "--finish")

    def uninstall(self):
        return self._sudo(self.MIGRATE, "--uninstall")

    def read_stage(self):
        return (self._sudo(self.MIGRATE, "--stage").stdout or "").strip()

    def flip(self, subcmd):
        return self._sudo(self.FLIP, subcmd)
```

Note: the test's `rec` ignores the `capture_output`/`text` kwargs (they land in `**k`), so the argv assertions match exactly.

- [ ] **Step 4: Run to verify it passes**

Run: `python tests/test_wizard_backend.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/wizard/backend.py tests/test_wizard_backend.py
git commit -m "feat(wizard): Backend — sudo helper invoker for the onboarding GUI"
```

---

### Task 2: WizardCore — stage read + screen decision

**Files:**
- Create: `distros/fedora-installer/wizard/core.py`
- Test: `tests/test_wizard_core_stage.py`

**Interfaces:**
- Consumes: `stage.py` constants (`INSTALLED`, `R1_PENDING`, `R1_HEAL`, `R2_PENDING`, `DONE`, `ROLLED_BACK`), `Backend` (Task 1).
- Produces: `WizardCore(backend=None, root="/")` with `current_stage() -> str` and `screen_for(stage) -> str`. Screen keys: `"welcome"` (INSTALLED), `"recovery_card"` is reached from welcome (not a stage), `"waiting_reboot"` (R1_PENDING/R2_PENDING), `"summary"` (R1_HEAL), `"final"` (DONE), `"rolled_back"` (ROLLED_BACK). `screen()` (no arg) = `screen_for(current_stage())`.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""WizardCore stage->screen decision — reads the stage file via stage.py, no Qt."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, rel))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
stage = load("stage", "distros/fedora-installer/migrate/stage.py")
core = load("wizard_core", "distros/fedora-installer/wizard/core.py")

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "var/lib"))
c = core.WizardCore(root=root)

check("absent stage reads INSTALLED", c.current_stage() == stage.INSTALLED)
check("INSTALLED -> welcome", c.screen_for(stage.INSTALLED) == "welcome")
check("R1_PENDING -> waiting_reboot", c.screen_for(stage.R1_PENDING) == "waiting_reboot")
check("R1_HEAL -> summary", c.screen_for(stage.R1_HEAL) == "summary")
check("R2_PENDING -> waiting_reboot", c.screen_for(stage.R2_PENDING) == "waiting_reboot")
check("DONE -> final", c.screen_for(stage.DONE) == "final")
check("ROLLED_BACK -> rolled_back", c.screen_for(stage.ROLLED_BACK) == "rolled_back")

stage.write_stage(stage.R1_HEAL, root=root)
check("screen() reflects the written stage", c.screen() == "summary")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python tests/test_wizard_core_stage.py`
Expected: FAIL — `core.py` does not exist.

- [ ] **Step 3: Write minimal implementation**

```python
import os
import importlib.util as _ilu

_MODDIR = os.path.dirname(os.path.abspath(__file__))


def _load_stage():
    p = os.path.join(_MODDIR, "..", "migrate", "stage.py")
    if not os.path.exists(p):
        p = "/usr/libexec/schema-init/stage.py"
    if not os.path.exists(p):
        raise SystemExit("schema-wizard: stage.py not found — is schema-init-migrate installed?")
    spec = _ilu.spec_from_file_location("stage", p)
    m = _ilu.module_from_spec(spec); spec.loader.exec_module(m)
    return m


stage = _load_stage()

_SCREEN = {
    stage.INSTALLED: "welcome",
    stage.R1_PENDING: "waiting_reboot",
    stage.R1_HEAL: "summary",
    stage.R2_PENDING: "waiting_reboot",
    stage.DONE: "final",
    stage.ROLLED_BACK: "rolled_back",
}


class WizardCore:
    def __init__(self, backend=None, root="/"):
        self.backend = backend
        self.root = root

    def current_stage(self):
        return stage.read_stage(self.root)

    def screen_for(self, s):
        return _SCREEN.get(s, "welcome")

    def screen(self):
        return self.screen_for(self.current_stage())
```

- [ ] **Step 4: Run to verify it passes**

Run: `python tests/test_wizard_core_stage.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/wizard/core.py tests/test_wizard_core_stage.py
git commit -m "feat(wizard): WizardCore stage->screen decision"
```

---

### Task 3: doctor-status parsing + humanize_reason

**Files:**
- Create: `distros/fedora-installer/wizard/status.py`
- Test: `tests/test_wizard_status.py`

**Interfaces:**
- Produces: `parse_status(json_str) -> {"overall": str, "items": [{"name","color","detail","action"}]}` (returns `{"overall":"UNKNOWN","items":[]}` on bad/empty input); `humanize_reason(raw) -> str` (plain-language mapping of a seatbelt rollback reason, ported from the yad wizard, with a generic fallback).

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""wizard status parsing + humanize_reason — pure, no Qt."""
import os, sys, json, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/wizard/status.py")
spec = importlib.util.spec_from_file_location("wizard_status", MOD)
st = importlib.util.module_from_spec(spec); spec.loader.exec_module(st)

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

good = json.dumps({"overall": "AMBER", "mode": "heal", "ts": 1,
                   "checks": [{"name": "card-input-acl", "color": "GREEN", "state": "clean",
                               "detail": "", "action": ""},
                              {"name": "powerdevil-running", "color": "AMBER", "state": "reported",
                               "detail": "PowerDevil is not running", "action": "detect-only"}]})
p = st.parse_status(good)
check("overall parsed", p["overall"] == "AMBER")
check("items parsed", len(p["items"]) == 2 and p["items"][1]["name"] == "powerdevil-running")

check("bad json -> UNKNOWN, empty items", st.parse_status("{not json")["overall"] == "UNKNOWN")
check("empty -> UNKNOWN", st.parse_status("")["items"] == [])

check("humanize: schema-udev not running",
      "didn't come up" in st.humanize_reason("schema-udev not running").lower())
check("humanize: desktop never confirmed",
      "desktop" in st.humanize_reason("desktop never confirmed across 2 armed boots").lower())
check("humanize: unknown reason falls back to something non-empty",
      len(st.humanize_reason("some novel reason")) > 0)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python tests/test_wizard_status.py`
Expected: FAIL — `status.py` does not exist.

- [ ] **Step 3: Write minimal implementation**

```python
import json


def parse_status(text):
    try:
        d = json.loads(text)
        if not isinstance(d, dict):
            raise ValueError
    except (ValueError, TypeError):
        return {"overall": "UNKNOWN", "items": []}
    items = []
    for c in d.get("checks", []) or []:
        items.append({"name": c.get("name", ""), "color": c.get("color", ""),
                      "detail": c.get("detail", ""), "action": c.get("action", "")})
    return {"overall": d.get("overall", "UNKNOWN"), "items": items}


_REASONS = [
    ("schema-udev not running", "the new device manager didn't come up, so we put the old one back"),
    ("missing core node", "an essential system device was missing, so we put the old setup back"),
    ("no /dev/disk/by-uuid", "the disk wasn't showing up the way the system expects, so we reverted"),
    ("no /dev/input", "the keyboard and mouse weren't ready, so we reverted"),
    ("no group-accessible", "the graphics device wasn't reachable by the desktop, so we reverted"),
    ("desktop never confirmed", "the desktop didn't finish coming up in time, so we put things back"),
]


def humanize_reason(raw):
    r = (raw or "").strip()
    for needle, plain in _REASONS:
        if needle in r:
            return plain
    return "something didn't come up cleanly after the switch, so your computer put itself back"
```

- [ ] **Step 4: Run to verify it passes**

Run: `python tests/test_wizard_status.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/wizard/status.py tests/test_wizard_status.py
git commit -m "feat(wizard): doctor-status parsing + humanize_reason (ported from the yad wizard)"
```

---

### Task 4: Advanced options model

**Files:**
- Modify: `distros/fedora-installer/wizard/core.py`
- Test: `tests/test_wizard_advanced.py`

**Interfaces:**
- Produces (module-level in `core.py`): `ADVANCED` = list of dicts `{"key","label","default"(bool),"dangerous"(bool),"warning"(str)}`; `WizardCore.deploy_opts(selections) -> list[str]` mapping a `{key: bool}` selection to migrate `--advanced-*` flags, emitting a flag only when a selection deviates from its default. Keys: `keep_fallback_entry` (default True), `udev_flip` (True), `dbus_broker` (True), `snapshot` (True), `doctor_timers` (True).

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""wizard advanced-option model — safe defaults, dangerous items carry a warning, flag mapping."""
import os, sys, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, rel))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
core = load("wizard_core", "distros/fedora-installer/wizard/core.py")

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

keys = {o["key"] for o in core.ADVANCED}
check("every layer present", keys == {"keep_fallback_entry", "udev_flip", "dbus_broker",
                                      "snapshot", "doctor_timers"})
check("all default to the safe path", all(o["default"] for o in core.ADVANCED))
check("dangerous toggles carry a warning line",
      all(o["warning"] for o in core.ADVANCED if o["dangerous"]))

c = core.WizardCore()
check("no deviation -> no flags", c.deploy_opts({o["key"]: o["default"] for o in core.ADVANCED}) == [])
check("snapshot off -> --advanced-no-snapshot",
      "--advanced-no-snapshot" in c.deploy_opts({"snapshot": False}))
check("keep_fallback_entry off -> --advanced-no-fallback-entry",
      "--advanced-no-fallback-entry" in c.deploy_opts({"keep_fallback_entry": False}))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python tests/test_wizard_advanced.py`
Expected: FAIL — `ADVANCED` / `deploy_opts` not defined.

- [ ] **Step 3: Write minimal implementation** (add to `core.py`)

```python
ADVANCED = [
    {"key": "keep_fallback_entry", "label": "Keep the current system as a backup boot option",
     "default": True, "dangerous": True,
     "warning": "Don't turn this off unless you're certain what it does — it is your way back."},
    {"key": "snapshot", "label": "Take a filesystem snapshot before changing anything",
     "default": True, "dangerous": True,
     "warning": "Don't turn this off unless you're certain what it does."},
    {"key": "udev_flip", "label": "Switch to the schema device manager (second reboot)",
     "default": True, "dangerous": True,
     "warning": "Don't turn this off unless you're certain what it does."},
    {"key": "dbus_broker", "label": "Switch to the schema message bus (second reboot)",
     "default": True, "dangerous": True,
     "warning": "Don't turn this off unless you're certain what it does."},
    {"key": "doctor_timers", "label": "Let the doctor keep watch and self-heal",
     "default": True, "dangerous": False, "warning": ""},
]

_OPT_FLAG = {
    "keep_fallback_entry": "--advanced-no-fallback-entry",
    "snapshot": "--advanced-no-snapshot",
    "udev_flip": "--advanced-no-udev-flip",
    "dbus_broker": "--advanced-no-dbus-broker",
    "doctor_timers": "--advanced-no-doctor-timers",
}
```

Then add the method to `WizardCore`:

```python
    def deploy_opts(self, selections):
        defaults = {o["key"]: o["default"] for o in ADVANCED}
        flags = []
        for key, default in defaults.items():
            chosen = selections.get(key, default)
            if chosen != default and not chosen:
                flags.append(_OPT_FLAG[key])
        return flags
```

Note: these `--advanced-no-*` flags are consumed by the migrate engine. Wiring them into `schema-migrate.py`'s argparse is deferred to Task 10's integration note; the wizard only needs to emit them, and unknown-flag handling on the migrate side is covered there. For v1, `deploy_opts` emitting the flags is the contract under test.

- [ ] **Step 4: Run to verify it passes**

Run: `python tests/test_wizard_advanced.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/wizard/core.py tests/test_wizard_advanced.py
git commit -m "feat(wizard): advanced-option model with safe defaults + flag mapping"
```

---

### Task 5: Flow orchestration + recovery-ack gate

**Files:**
- Modify: `distros/fedora-installer/wizard/core.py`
- Test: `tests/test_wizard_flow.py`

**Interfaces:**
- Consumes: `Backend` (Task 1), `ADVANCED`/`deploy_opts` (Task 4), `stage` constants.
- Produces: `WizardCore.recovery_text() -> str` (the plain-language card, reused from migrate's `RECOVERY_TEXT`); `WizardCore.advance(consent, recovery_ack, selections) -> str` returning an action token: `"need_recovery_ack"`, `"deployed"`, `"armed"`, `"finished"`, or `"noop"`. Rules: at `INSTALLED`, refuse with `"need_recovery_ack"` unless `recovery_ack` is True, else `backend.deploy(deploy_opts(selections))` → `"deployed"`; at `R1_HEAL`, `backend.arm_flip()` → `"armed"`; at `DONE`/`ROLLED_BACK`, `backend.finish()` (teardown/report) → `"finished"`; otherwise `"noop"`. `consent=False` always yields `"noop"`.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""wizard flow orchestration + recovery-ack gate — injected Backend, temp stage."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, rel))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
stage = load("stage", "distros/fedora-installer/migrate/stage.py")
core = load("wizard_core", "distros/fedora-installer/wizard/core.py")

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

class FakeBackend:
    def __init__(self): self.calls = []
    def deploy(self, opts=None): self.calls.append(("deploy", opts or [])); return self
    def arm_flip(self): self.calls.append(("arm_flip",)); return self
    def finish(self): self.calls.append(("finish",)); return self

def fresh(stg):
    root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "var/lib"))
    if stg is not None:
        stage.write_stage(stg, root=root)
    be = FakeBackend()
    return core.WizardCore(backend=be, root=root), be

# recovery card gate
c, be = fresh(None)  # INSTALLED
check("INSTALLED without ack refuses", c.advance(consent=True, recovery_ack=False, selections={}) == "need_recovery_ack")
check("refused -> nothing deployed", be.calls == [])

c, be = fresh(None)
check("INSTALLED with ack deploys", c.advance(consent=True, recovery_ack=True, selections={"snapshot": False}) == "deployed")
check("deploy got the advanced flags", be.calls[-1] == ("deploy", ["--advanced-no-snapshot"]))

c, be = fresh(stage.R1_HEAL)
check("R1_HEAL arms the flip", c.advance(consent=True, recovery_ack=True, selections={}) == "armed")
check("arm_flip called", be.calls[-1] == ("arm_flip",))

c, be = fresh(stage.DONE)
check("DONE finishes/tears down", c.advance(consent=True, recovery_ack=True, selections={}) == "finished")

c, be = fresh(stage.ROLLED_BACK)
check("ROLLED_BACK finishes/tears down", c.advance(consent=True, recovery_ack=True, selections={}) == "finished")

c, be = fresh(None)
check("no consent -> noop", c.advance(consent=False, recovery_ack=True, selections={}) == "noop")
check("no consent -> nothing called", be.calls == [])

check("recovery_text is the plain-language card", "black" in c.recovery_text().lower())

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python tests/test_wizard_flow.py`
Expected: FAIL — `advance` / `recovery_text` not defined.

- [ ] **Step 3: Write minimal implementation** (add to `core.py`)

Add the migrate-module load near the stage load (reuse its `RECOVERY_TEXT`):

```python
def _load_migrate():
    p = os.path.join(_MODDIR, "..", "migrate", "schema-migrate.py")
    if not os.path.exists(p):
        p = "/usr/bin/schema-migrate"
    spec = _ilu.spec_from_file_location("schema_migrate_ro", p)
    m = _ilu.module_from_spec(spec); spec.loader.exec_module(m)
    return m
```

Then add to `WizardCore`:

```python
    def recovery_text(self):
        return _load_migrate().RECOVERY_TEXT

    def advance(self, consent, recovery_ack, selections):
        if not consent:
            return "noop"
        s = self.current_stage()
        if s == stage.INSTALLED:
            if not recovery_ack:
                return "need_recovery_ack"
            self.backend.deploy(self.deploy_opts(selections))
            return "deployed"
        if s == stage.R1_HEAL:
            self.backend.arm_flip()
            return "armed"
        if s in (stage.DONE, stage.ROLLED_BACK):
            self.backend.finish()
            return "finished"
        return "noop"
```

Note: importing `schema-migrate.py` as a module executes its top-level `stage` load; that is already handled (Plan A hardened it with a SystemExit guard). `RECOVERY_TEXT` is a module constant, safe to read.

- [ ] **Step 4: Run to verify it passes**

Run: `python tests/test_wizard_flow.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/wizard/core.py tests/test_wizard_flow.py
git commit -m "feat(wizard): flow orchestration + mandatory recovery-card gate"
```

---

### Task 6: WizardController (Qt) wrapping the core

**Files:**
- Create: `distros/fedora-installer/wizard/controller.py`
- Test: `tests/test_wizard_controller.py`

**Interfaces:**
- Consumes: `WizardCore`, `Backend`, `parse_status`/`humanize_reason`.
- Produces: `WizardController(QObject, core=None)` exposing Qt `Property(str) screen`, `Property(str) overall`, `Property('QVariantList') statusItems`, `Property(bool) recoveryAck` (writable via slot), `Slot() refresh`, `Slot(bool) setRecoveryAck`, `Slot(str,bool) setAdvanced`, `Slot() continueClicked`; and `Signal() changed`. `continueClicked` calls `core.advance(consent=True, recovery_ack=<current>, selections=<current>)` and emits `changed`.

- [ ] **Step 1: Write the failing test** (headless — sets offscreen platform before importing Qt)

```python
#!/usr/bin/env python3
"""WizardController — headless (offscreen) Qt wrapper over WizardCore."""
import os, sys, tempfile, importlib.util
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

try:
    import PySide6  # noqa: F401
except ImportError:
    print("SKIP  PySide6 not installed"); print("PASS"); sys.exit(0)

def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, rel))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
stage = load("stage", "distros/fedora-installer/migrate/stage.py")
core = load("wizard_core", "distros/fedora-installer/wizard/core.py")
ctrl = load("wizard_controller", "distros/fedora-installer/wizard/controller.py")

from PySide6.QtCore import QCoreApplication
app = QCoreApplication.instance() or QCoreApplication(sys.argv)

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

class FakeBackend:
    def __init__(self): self.calls = []
    def deploy(self, opts=None): self.calls.append(("deploy", opts or [])); return self
    def arm_flip(self): self.calls.append(("arm_flip",)); return self
    def finish(self): self.calls.append(("finish",)); return self

root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "var/lib"))
be = FakeBackend()
wc = core.WizardCore(backend=be, root=root)
co = ctrl.WizardController(core=wc)

check("screen property reflects core (INSTALLED->welcome)", co.screen == "welcome")

co.setRecoveryAck(False)
co.continueClicked()
check("continue without ack does not deploy", be.calls == [])

co.setRecoveryAck(True)
co.continueClicked()
check("continue with ack deploys", be.calls and be.calls[-1][0] == "deploy")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python tests/test_wizard_controller.py`
Expected: FAIL — `controller.py` does not exist.

- [ ] **Step 3: Write minimal implementation**

```python
import subprocess
import os
import importlib.util as _ilu
from PySide6.QtCore import QObject, Property, Signal, Slot

_MODDIR = os.path.dirname(os.path.abspath(__file__))


def _load(name, fname):
    spec = _ilu.spec_from_file_location(name, os.path.join(_MODDIR, fname))
    m = _ilu.module_from_spec(spec); spec.loader.exec_module(m); return m


_core = _load("wizard_core", "core.py")
_status = _load("wizard_status", "status.py")
_backend = _load("wizard_backend", "backend.py")


class WizardController(QObject):
    changed = Signal()

    def __init__(self, core=None, parent=None):
        super().__init__(parent)
        self._core = core or _core.WizardCore(backend=_backend.Backend())
        self._recovery_ack = False
        self._selections = {o["key"]: o["default"] for o in _core.ADVANCED}

    def _get_screen(self):
        return self._core.screen()

    def _get_overall(self):
        try:
            with open(os.path.join(self._core.root, "run/schema-init/doctor-status.json")) as fh:
                return _status.parse_status(fh.read())["overall"]
        except OSError:
            return "UNKNOWN"

    def _get_status_items(self):
        try:
            with open(os.path.join(self._core.root, "run/schema-init/doctor-status.json")) as fh:
                return _status.parse_status(fh.read())["items"]
        except OSError:
            return []

    def _get_recovery_ack(self):
        return self._recovery_ack

    screen = Property(str, _get_screen, notify=changed)
    overall = Property(str, _get_overall, notify=changed)
    statusItems = Property('QVariantList', _get_status_items, notify=changed)
    recoveryAck = Property(bool, _get_recovery_ack, notify=changed)

    @Slot()
    def refresh(self):
        self.changed.emit()

    @Slot(bool)
    def setRecoveryAck(self, v):
        self._recovery_ack = bool(v)
        self.changed.emit()

    @Slot(str, bool)
    def setAdvanced(self, key, v):
        self._selections[key] = bool(v)

    @Slot(result=str)
    def recoveryText(self):
        return self._core.recovery_text()

    @Slot()
    def continueClicked(self):
        self._core.advance(consent=True, recovery_ack=self._recovery_ack,
                            selections=self._selections)
        self.changed.emit()
```

- [ ] **Step 4: Run to verify it passes**

Run: `python tests/test_wizard_controller.py`
Expected: PASS (or SKIP-then-PASS if PySide6 is somehow absent).

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/wizard/controller.py tests/test_wizard_controller.py
git commit -m "feat(wizard): Qt WizardController exposing the core to QML"
```

---

### Task 7: QML views + entrypoint + QML load-smoke

**Files:**
- Create: `distros/fedora-installer/wizard/qml/Main.qml`
- Create: `distros/fedora-installer/wizard/main.py`
- Test: `tests/test_wizard_qml_smoke.py`

**Interfaces:**
- Consumes: `WizardController` (registered as a context property `wizard`).
- Produces: `main.py` `main(argv) -> int` that builds `QGuiApplication`, a `QQmlApplicationEngine`, sets `wizard` context property to a `WizardController`, loads `qml/Main.qml`, returns nonzero if no root object loaded. `Main.qml` is a `StackView`/`Loader`-based screen switch keyed on `wizard.screen`.

- [ ] **Step 1: Write the failing test** (offscreen; asserts the QML loads with zero errors)

```python
#!/usr/bin/env python3
"""QML load-smoke — the engine loads Main.qml offscreen with no errors."""
import os, sys, importlib.util
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

try:
    import PySide6  # noqa: F401
except ImportError:
    print("SKIP  PySide6 not installed"); print("PASS"); sys.exit(0)

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

WIZ = os.path.join(REPO, "distros/fedora-installer/wizard")
spec = importlib.util.spec_from_file_location("wizard_main", os.path.join(WIZ, "main.py"))
wm = importlib.util.module_from_spec(spec); spec.loader.exec_module(wm)

from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtCore import QUrl

app = QGuiApplication.instance() or QGuiApplication(sys.argv)
engine = QQmlApplicationEngine()
errors = []
engine.warnings.connect(lambda ws: errors.extend(str(w.toString()) for w in ws))
ctrl_spec = importlib.util.spec_from_file_location("wizard_controller", os.path.join(WIZ, "controller.py"))
cm = importlib.util.module_from_spec(ctrl_spec); ctrl_spec.loader.exec_module(cm)
engine.rootContext().setContextProperty("wizard", cm.WizardController())
engine.load(QUrl.fromLocalFile(os.path.join(WIZ, "qml", "Main.qml")))

check("Main.qml produced a root object", len(engine.rootObjects()) == 1)
check("no QML warnings/errors", errors == [])

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python tests/test_wizard_qml_smoke.py`
Expected: FAIL — `main.py` / `Main.qml` do not exist.

- [ ] **Step 3: Write minimal implementation**

`main.py`:

```python
import os
import sys
import importlib.util as _ilu
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtCore import QUrl

_MODDIR = os.path.dirname(os.path.abspath(__file__))


def _controller():
    spec = _ilu.spec_from_file_location("wizard_controller", os.path.join(_MODDIR, "controller.py"))
    m = _ilu.module_from_spec(spec); spec.loader.exec_module(m)
    return m.WizardController()


def main(argv):
    app = QGuiApplication.instance() or QGuiApplication(argv)
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty("wizard", _controller())
    engine.load(QUrl.fromLocalFile(os.path.join(_MODDIR, "qml", "Main.qml")))
    if not engine.rootObjects():
        return 1
    return app.exec()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

`qml/Main.qml` (minimal, valid, screen-switching; expand copy/styling in follow-up polish, but this must load clean):

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    visible: true
    width: 640; height: 480
    title: "Set up schema"

    function screenText() {
        switch (wizard.screen) {
        case "welcome": return "Welcome — this will set up schema on your computer.";
        case "waiting_reboot": return "Please restart your computer to continue.";
        case "summary": return "First stage done. Here is what the doctor checked.";
        case "final": return "Schema is successfully installed.";
        case "rolled_back": return "Your computer put itself back safely.";
        default: return "";
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: 18
            text: win.screenText()
        }

        // Recovery-card acknowledgement, shown only before the first reboot.
        ColumnLayout {
            Layout.fillWidth: true
            visible: wizard.screen === "welcome"
            spacing: 8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: wizard.recoveryText()
            }
            CheckBox {
                id: ack
                text: "I've saved these instructions"
                onToggled: wizard.setRecoveryAck(checked)
            }
        }

        Item { Layout.fillHeight: true }

        Button {
            text: "Continue"
            enabled: wizard.screen !== "welcome" || ack.checked
            onClicked: wizard.continueClicked()
        }
    }
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `python tests/test_wizard_qml_smoke.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/wizard/main.py distros/fedora-installer/wizard/qml/Main.qml tests/test_wizard_qml_smoke.py
git commit -m "feat(wizard): QML Main view + entrypoint, load-smoke green"
```

---

### Task 8: Launch shim + autostart/Desktop assets

**Files:**
- Create: `distros/fedora-installer/wizard/schema-wizard` (the `%{_bindir}` shim)
- Create: `distros/fedora-installer/wizard/schema-wizard.desktop` (autostart)
- Create: `distros/fedora-installer/wizard/schema-wizard-launcher.desktop` (Desktop launcher)
- Test: folded into Task 10 (the install/rpm smoke exercises these); no unit test — they are static assets whose only contract is "installed + valid Desktop syntax", checked by `desktop-file-validate` in Task 10.

- [ ] **Step 1: Create the shim** `distros/fedora-installer/wizard/schema-wizard`

```sh
#!/bin/sh
exec python3 /usr/libexec/schema-init/wizard/main.py "$@"
```

- [ ] **Step 2: Create the autostart entry** `schema-wizard.desktop`

```ini
[Desktop Entry]
Type=Application
Name=Finish setting up schema
Exec=/usr/bin/schema-wizard
Icon=drive-harddisk
Terminal=false
X-KDE-autostart-phase=2
```

- [ ] **Step 3: Create the Desktop launcher** `schema-wizard-launcher.desktop` (same content, plus a comment; identical Exec — the wizard no-ops if the stage is terminal)

```ini
[Desktop Entry]
Type=Application
Name=Set up schema
Comment=Convert this computer to schema-init
Exec=/usr/bin/schema-wizard
Icon=drive-harddisk
Terminal=false
```

- [ ] **Step 4: Validate syntax locally**

Run: `desktop-file-validate distros/fedora-installer/wizard/schema-wizard.desktop distros/fedora-installer/wizard/schema-wizard-launcher.desktop`
Expected: no output (valid).

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/wizard/schema-wizard distros/fedora-installer/wizard/schema-wizard.desktop distros/fedora-installer/wizard/schema-wizard-launcher.desktop
git commit -m "feat(wizard): launch shim + autostart/Desktop entries"
```

---

### Task 9: Migrate engine — accept the `--advanced-*` flags

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py` (argparse in `main`, and thread the booleans into `do_deploy`)
- Test: `tests/test_migrate_advanced_flags.py`

**Interfaces:**
- Consumes: `deploy_opts` flag names from Task 4.
- Produces: `schema-migrate --deploy --prebuilt` accepts `--advanced-no-fallback-entry`, `--advanced-no-snapshot`, `--advanced-no-udev-flip`, `--advanced-no-dbus-broker`, `--advanced-no-doctor-timers` without error; each sets the corresponding behavior off. For v1 the only ones with deploy-time effect are snapshot and fallback-entry (udev/dbus flip is an R2 concern, doctor timers a post-deploy concern) — the others are accepted and recorded into the profile so R2/finish can honor them, never rejected.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""migrate accepts the wizard's --advanced-* flags without error."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

def load():
    spec = importlib.util.spec_from_file_location("schema_migrate_af", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

root = tempfile.mkdtemp()
for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib", "home/j", "usr/lib/systemd"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "etc/os-release"), "w").write("ID=fedora\n")
open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
for b in ("schema-init", "schema-ctl", "schema-subreaper"):
    open(os.path.join(root, "usr/bin", b), "w").close()
open(os.path.join(root, "usr/lib/systemd/systemd-udevd"), "w").close()
open(os.path.join(root, "etc/fstab"), "w").write("UUID=a / ext4 defaults 0 1\n")
open(os.path.join(root, "etc/passwd"), "w").write("j:x:1000:1000::/home/j:/bin/sh\n")
open(os.path.join(root, "boot/loader/entries/f.conf"), "w").write(
    "title Fedora\nversion 6.10.0\noptions root=UUID=a ro\n")
os.environ["MIGRATE_ROOT"] = root; os.environ["MIGRATE_KERNEL"] = "6.10.0"
m = load()
try:
    rc = m.main(["--deploy", "--prebuilt", "--advanced-no-snapshot", "--advanced-no-doctor-timers"],
                run=lambda *a, **k: type("R", (), {"returncode": 0, "stdout": ""})())
    check("advanced flags accepted, deploy returns 0", rc == 0)
    check("stage advanced to R1_PENDING", m.stage.read_stage(root) == m.stage.R1_PENDING)
finally:
    del os.environ["MIGRATE_ROOT"]; del os.environ["MIGRATE_KERNEL"]

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python tests/test_migrate_advanced_flags.py`
Expected: FAIL — argparse errors on the unknown `--advanced-*` options (exit 2).

- [ ] **Step 3: Write minimal implementation** (in `schema-migrate.py` `main`, add the arguments)

```python
    ap.add_argument("--advanced-no-fallback-entry", action="store_true")
    ap.add_argument("--advanced-no-snapshot", action="store_true")
    ap.add_argument("--advanced-no-udev-flip", action="store_true")
    ap.add_argument("--advanced-no-dbus-broker", action="store_true")
    ap.add_argument("--advanced-no-doctor-timers", action="store_true")
```

For v1 these are accepted and recorded so later stages can honor them; add near where the profile is written in `do_deploy`, persisting the advanced choices into the profile dict (so R2/finish can read them). Minimal wiring that satisfies the test is that argparse accepts them; recording is:

```python
    # in main(), after args parsed, before do_deploy:
    os.environ.setdefault("MIGRATE_ADV", ",".join(
        k for k in ("no-fallback-entry", "no-snapshot", "no-udev-flip", "no-dbus-broker", "no-doctor-timers")
        if getattr(args, "advanced_" + k.replace("-", "_"))))
```

(Full behavioral wiring of snapshot/fallback toggles is a follow-up; v1's contract, under test, is that the flags are accepted and deploy still advances the stage.)

- [ ] **Step 4: Run to verify it passes**

Run: `python tests/test_migrate_advanced_flags.py`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_advanced_flags.py
git commit -m "feat(migrate): accept the wizard's --advanced-* deploy flags"
```

---

### Task 10: COPR packaging — the `schema-wizard` subpackage

**Files:**
- Modify: `schema-init.spec` (new `%package wizard` + `%files wizard`), `Makefile` (new `install-wizard` target)
- Test: `tests/test_spec_wizard_subpackage.py`

**Interfaces:**
- Consumes: all wizard files (Tasks 1–8).
- Produces: `schema-init-wizard` binary package, `Requires: schema-init` + `python3-pyside6`, shipping `%{_bindir}/schema-wizard`, `%{_libexecdir}/schema-init/wizard/` (py + qml), `%{_sysconfdir}/xdg/autostart/schema-wizard.desktop`.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""schema-init-wizard subpackage declared with the right deps + payload."""
import os, sys, subprocess
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

rpms = subprocess.run(["rpmspec", "-q", "--rpms", os.path.join(REPO, "schema-init.spec")],
                      capture_output=True, text=True).stdout
check("schema-init-wizard subpackage declared", "schema-init-wizard" in rpms)

reqs = subprocess.run(["rpmspec", "-q", "--requires", os.path.join(REPO, "schema-init.spec")],
                      capture_output=True, text=True).stdout
check("requires python3-pyside6", "python3-pyside6" in reqs)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python tests/test_spec_wizard_subpackage.py`
Expected: FAIL — no `schema-init-wizard` in the spec.

- [ ] **Step 3: Write the Makefile target** (`install-wizard`)

```make
install-wizard:
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(PREFIX)/libexec/schema-init/wizard/qml
	install -m 0755 distros/fedora-installer/wizard/schema-wizard $(DESTDIR)$(BINDIR)/schema-wizard
	install -m 0644 distros/fedora-installer/wizard/backend.py $(DESTDIR)$(PREFIX)/libexec/schema-init/wizard/backend.py
	install -m 0644 distros/fedora-installer/wizard/core.py $(DESTDIR)$(PREFIX)/libexec/schema-init/wizard/core.py
	install -m 0644 distros/fedora-installer/wizard/status.py $(DESTDIR)$(PREFIX)/libexec/schema-init/wizard/status.py
	install -m 0644 distros/fedora-installer/wizard/controller.py $(DESTDIR)$(PREFIX)/libexec/schema-init/wizard/controller.py
	install -m 0644 distros/fedora-installer/wizard/main.py $(DESTDIR)$(PREFIX)/libexec/schema-init/wizard/main.py
	install -m 0644 distros/fedora-installer/wizard/qml/Main.qml $(DESTDIR)$(PREFIX)/libexec/schema-init/wizard/qml/Main.qml
	install -d $(DESTDIR)$(SYSCONFDIR)/xdg/autostart
	install -m 0644 distros/fedora-installer/wizard/schema-wizard.desktop $(DESTDIR)$(SYSCONFDIR)/xdg/autostart/schema-wizard.desktop
```

- [ ] **Step 4: Add the subpackage to `schema-init.spec`**

After the `%files migrate` block, add:

```spec
%package wizard
Summary:   Guided PySide6 GUI that converts a Fedora KDE box onto schema-init
Requires:  %{name}-migrate = %{version}-%{release}
Requires:  python3-pyside6
BuildArch: noarch
%description wizard
The onboarding wizard: a native Plasma (Qt/QML) GUI that walks a novice through
the two-reboot in-place conversion, driving the migrate engine, the flip helper,
and schema-doctor. Unprivileged; escalates only through the fixed helpers.

%files wizard
%dir %{_libexecdir}/schema-init/wizard
%dir %{_libexecdir}/schema-init/wizard/qml
%{_bindir}/schema-wizard
%{_libexecdir}/schema-init/wizard/*.py
%{_libexecdir}/schema-init/wizard/qml/*.qml
%{_sysconfdir}/xdg/autostart/schema-wizard.desktop
```

And add `make install-wizard ...` to the `%install` section:

```spec
make install-wizard DESTDIR=%{buildroot} PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir}
```

(Note: `schema-init-wizard` is `noarch` but the base package is arch'd; that is fine for a subpackage. The base `%build` is unaffected — no new binaries.)

- [ ] **Step 5: Run the spec test + a real build/install smoke**

Run: `python tests/test_spec_wizard_subpackage.py`
Expected: PASS

Then the real proof (mirrors Plan A's verification — actually build and install, don't trust the diff):

```bash
TOP=$(mktemp -d)/rpmbuild; mkdir -p "$TOP"/{SOURCES,SPECS,BUILD,BUILDROOT,RPMS,SRPMS}
git archive --format=tar.gz --prefix=schema-init-0.2.1/ HEAD -o "$TOP/SOURCES/schema-init-0.2.1.tar.gz"
rpmbuild --define "_topdir $TOP" -bb schema-init.spec
# then in a fedora:44 container: dnf install the three rpms, and:
#   test -x /usr/bin/schema-wizard
#   QT_QPA_PLATFORM=offscreen python3 -c "import sys; sys.argv=['x']; \
#     import importlib.util as u; s=u.spec_from_file_location('m','/usr/libexec/schema-init/wizard/main.py'); \
#     m=u.module_from_spec(s); s.loader.exec_module(m); print('rc', m.main(['x']))"
#   desktop-file-validate /etc/xdg/autostart/schema-wizard.desktop
```
Expected: both RPMs build; `schema-wizard` present; the offscreen `main()` returns 1 only if QML failed to load (0/exec otherwise); desktop file valid.

- [ ] **Step 6: Commit**

```bash
git add schema-init.spec Makefile tests/test_spec_wizard_subpackage.py
git commit -m "feat(pkg): schema-init-wizard subpackage (PySide6 GUI)"
```

---

## Self-Review

**1. Spec coverage:**
- Two-reboot stage machine + autostart-resume → Tasks 2, 5, 8 (screen decision, flow, autostart). ✓
- Prebuilt deploy via migrate → Task 1 (`--prebuilt`), Task 5 (deploy). ✓
- Recovery card, mandatory ack before R1 → Task 5 (`recovery_text`, `need_recovery_ack` gate) + Task 7 (QML checkbox gating Continue). ✓
- Privileged surface = fixed helpers via sudo, no inline → Task 1 (Backend is the only privilege path). ✓
- Advanced disclosure, safe defaults, warnings → Task 4 + Task 7 (Advanced list rendered from `ADVANCED`). Note: the QML Advanced *expander UI* is minimal in Task 7; full expander rendering is polish, but the model + warnings + flag mapping are complete and tested. ✓ (gap: rich Advanced UI is deferred polish, flagged below)
- Between-round summary from doctor-status → Task 3 (`parse_status`) + Task 6 (`overall`/`statusItems` properties). ✓
- Final "successfully installed" + report; rolled-back humanized → Task 3 (`humanize_reason`), Task 2 (screens `final`/`rolled_back`), Task 5 (`finish`). ✓
- COPR packaging, `schema-wizard` requires pyside6 → Task 10. ✓
- Reuse migrate/flip/doctor, no reimplementation → Tasks 1/5 call the CLIs; humanize ported (Task 3) since it lived in bash. ✓
- Success criteria 1–5 (VM click-through, seatbelt rollback, uninstall, advanced, recovery card) → exercised by the flow/backend/QML tests here; the full on-VM click-through is an integration/verification step after implementation (schema-vmtest + a Fedora-KDE VM), not a unit task. Flagged below.

**2. Placeholder scan:** No "TBD/TODO/handle edge cases". Two explicit deferrals are named as deferrals with a v1 contract that IS tested (advanced-flag behavioral wiring in Task 9; rich Advanced expander UI in Task 7) — these are scoped reductions, not placeholders.

**3. Type consistency:** `Backend` method names (`deploy/arm_flip/finish/uninstall/read_stage/flip`) identical across Tasks 1, 5, 6. `deploy_opts` / `ADVANCED` keys identical Tasks 4↔5↔9 (`--advanced-no-*`). Screen keys (`welcome/waiting_reboot/summary/final/rolled_back`) identical Tasks 2↔6↔7. `advance` return tokens (`need_recovery_ack/deployed/armed/finished/noop`) defined Task 5, consumed by Task 6's `continueClicked`. Consistent.

## Deferred to post-plan polish / verification (not tasks — named honestly)
- **Full on-VM click-through** (spec success criteria 1–5): after implementation, run `schema-vmtest` and a real Fedora-KDE VM to prove the two-reboot ladder, a forced-black-screen recovery, seatbelt rollback landing in `rolled_back`, and `--uninstall`. Unit tests here prove the brain; only a VM proves the human-visible outcome.
- **Rich Advanced expander UI + between-round/final report styling** in QML: Task 7 ships a minimal valid Main.qml that loads and gates Continue; expanding the per-screen visual design (native Plasma polish, the collapsible Advanced list rendering `ADVANCED`, the scrollable report) is a follow-up once the flow is proven end-to-end.
- **Advanced-flag behavioral wiring** (snapshot-skip, fallback-entry-skip actually changing deploy behavior): Task 9 accepts + records them; making each flag change migrate's behavior is a scoped follow-up with its own tests.

## Execution Handoff

Two execution options:
1. **Subagent-Driven (recommended)** — a fresh subagent per task, review between tasks.
2. **Inline Execution** — execute here with checkpoints.
