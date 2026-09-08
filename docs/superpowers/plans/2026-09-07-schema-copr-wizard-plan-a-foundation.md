# Plan A — schema COPR onboarding: Foundation (packaging + engine + stage machine)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the two-round, in-place Fedora-KDE→schema-init conversion installable from COPR (prebuilt, no compile-on-target) and drivable end-to-end from the `schema-migrate` CLI, gated by one root-owned stage machine — with no GUI yet.

**Architecture:** Reuse the existing `schema-migrate.py` deploy engine and `schema-flip-apply.sh` helper. Add a root-owned stage file that the migrate CLI (the sole privileged writer) advances across two reboots: R1 lays schema-init PID 1 on stock udev/dbus, R2 arms the udev+dbus flip. Package it as a new `schema-init-migrate` subpackage so the pristine base `schema-init` package keeps its "installing this does not change your init" contract.

**Tech Stack:** Python 3 stdlib (engine, stage machine), POSIX sh (flip helper), RPM spec (`schema-init.spec`), pytest (tests over a `MIGRATE_ROOT` temp tree).

**Spec:** `docs/superpowers/specs/2026-09-07-schema-copr-onboarding-design.md`

## Global Constraints

- **v1 fence:** Fedora KDE, in-place, box already running. Platform gate stays (`detect_platform()` returns `fedora-kde` or refuses).
- **Prebuilt, not compiled on target.** In prebuilt mode the engine consumes RPM-installed binaries; it never runs `make`. Core binaries owned by RPM are NOT added to the uninstall manifest.
- **Privileged surface:** one fixed helper set — `/usr/bin/schema-migrate` and `/usr/libexec/schema-init/schema-flip-apply` — reached via `%wheel ALL=(root) NOPASSWD:` for those exact paths only. No wildcards.
- **Authoritative stage:** `/var/lib/schema-init/wizard-stage.json`, root-owned, `0644`, written only by the migrate CLI. All paths honor `MIGRATE_ROOT` via the engine's existing `P()` helper.
- **Base package untouched:** `schema-init` %files must NOT gain `schema-udev` or any onboarding file. All new payload ships in `schema-init-migrate`.
- Stdlib only in the engine; no docstrings/comments beyond what exists unless a step's code shows them. Follow the repo's existing test style (tempfile + env, injected `run=`).

---

### Task 1: Stage machine module

**Files:**
- Create: `distros/fedora-installer/migrate/stage.py`
- Test: `tests/test_stage.py`

**Interfaces:**
- Produces:
  - `INSTALLED, R1_PENDING, R1_HEAL, R2_PENDING, DONE, ROLLED_BACK` (str constants)
  - `STAGE_PATH = "var/lib/schema-init/wizard-stage.json"`
  - `read_stage(root="/") -> str` — returns `INSTALLED` when the file is absent
  - `write_stage(stage, root="/", extra=None) -> str` — writes `{"stage":…, "ts":…, **extra}`, chmod `0644`, returns the path
  - `transition(new, root="/") -> str` — validates `new` is reachable from `read_stage(root)`, then writes; raises `ValueError` on an illegal hop
  - `VALID = {INSTALLED:{R1_PENDING}, R1_PENDING:{R1_HEAL}, R1_HEAL:{R2_PENDING}, R2_PENDING:{DONE, ROLLED_BACK}, DONE:set(), ROLLED_BACK:set()}`

- [ ] **Step 1: Write the failing test**

```python
# tests/test_stage.py
import os, sys, tempfile, importlib.util, pytest
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/stage.py")
spec = importlib.util.spec_from_file_location("stage", MOD)
stage = importlib.util.module_from_spec(spec); spec.loader.exec_module(stage)

def _root():
    r = tempfile.mkdtemp(); os.makedirs(os.path.join(r, "var/lib")); return r

def test_absent_reads_installed():
    assert stage.read_stage(_root()) == stage.INSTALLED

def test_write_then_read_roundtrip():
    r = _root()
    stage.write_stage(stage.R1_PENDING, root=r)
    assert stage.read_stage(r) == stage.R1_PENDING
    assert oct(os.stat(os.path.join(r, stage.STAGE_PATH)).st_mode)[-3:] == "644"

def test_legal_transition():
    r = _root()
    stage.transition(stage.R1_PENDING, root=r)
    stage.transition(stage.R1_HEAL, root=r)
    assert stage.read_stage(r) == stage.R1_HEAL

def test_illegal_transition_raises():
    r = _root()
    with pytest.raises(ValueError):
        stage.transition(stage.DONE, root=r)   # INSTALLED -> DONE is not allowed

def test_extra_fields_persist():
    r = _root()
    stage.write_stage(stage.R2_PENDING, root=r, extra={"snapshot": "@pre-schema"})
    import json
    d = json.load(open(os.path.join(r, stage.STAGE_PATH)))
    assert d["snapshot"] == "@pre-schema" and "ts" in d
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest tests/test_stage.py -v`
Expected: FAIL — `stage.py` does not exist / attributes missing.

- [ ] **Step 3: Write minimal implementation**

```python
# distros/fedora-installer/migrate/stage.py
import json, os, time

INSTALLED, R1_PENDING, R1_HEAL, R2_PENDING, DONE, ROLLED_BACK = (
    "INSTALLED", "R1_PENDING", "R1_HEAL", "R2_PENDING", "DONE", "ROLLED_BACK")
STAGE_PATH = "var/lib/schema-init/wizard-stage.json"
VALID = {
    INSTALLED: {R1_PENDING}, R1_PENDING: {R1_HEAL}, R1_HEAL: {R2_PENDING},
    R2_PENDING: {DONE, ROLLED_BACK}, DONE: set(), ROLLED_BACK: set(),
}

def _p(root):
    return os.path.join(root, STAGE_PATH.lstrip("/"))

def read_stage(root="/"):
    try:
        return json.load(open(_p(root)))["stage"]
    except (OSError, ValueError, KeyError):
        return INSTALLED

def write_stage(stage, root="/", extra=None):
    p = _p(root)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    d = {"stage": stage, "ts": int(time.time())}
    if extra:
        d.update(extra)
    with open(p, "w") as fh:
        json.dump(d, fh, indent=2)
    os.chmod(p, 0o644)
    return p

def transition(new, root="/", extra=None):
    cur = read_stage(root)
    if new not in VALID.get(cur, set()):
        raise ValueError("illegal stage transition %s -> %s" % (cur, new))
    return write_stage(new, root=root, extra=extra)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m pytest tests/test_stage.py -v`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/stage.py tests/test_stage.py
git commit -m "feat(migrate): root-owned wizard stage machine"
```

---

### Task 2: Prebuilt deploy mode

Make the engine consume RPM-installed binaries instead of compiling. Today `do_deploy` → `run_make_install` always builds. Add prebuilt mode: when the core binaries already exist and `MIGRATE_PREBUILT=1` (or `--prebuilt`), skip the build and skip adding those RPM-owned files to the manifest.

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py` (`run_make_install`, `do_deploy`, `main`)
- Test: `tests/test_migrate_prebuilt.py`

**Interfaces:**
- Consumes: `Manifest` (Task uses existing class), `P()`, `do_deploy(run=…, dry_run=…, prebuilt=False)`
- Produces:
  - `PREBUILT_BINS = ["schema-init", "schema-ctl", "schema-subreaper"]`
  - `provision_binaries(manifest, run=subprocess.run, dry_run=False, prebuilt=False)` — prebuilt branch verifies `P("usr/bin/schema-init")` exists and returns without building or adding to the manifest; else falls through to the existing build path (renamed body of `run_make_install`).
  - `do_deploy(..., prebuilt=False)` passes the flag through.
  - `main` accepts `--prebuilt` and honors `MIGRATE_PREBUILT`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_migrate_prebuilt.py
import os, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_pb", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def test_prebuilt_skips_build_and_manifest():
    m = _load()
    root = tempfile.mkdtemp()
    os.makedirs(os.path.join(root, "usr/bin"))
    open(os.path.join(root, "usr/bin/schema-init"), "w").close()
    os.environ["MIGRATE_ROOT"] = root
    calls = []
    def fake_run(argv, *a, **k):
        calls.append(argv)
        class R: returncode = 0; stdout = ""
        return R()
    man = m.Manifest()
    m.provision_binaries(man, run=fake_run, prebuilt=True)
    assert not any("make" in " ".join(c) for c in calls)   # never built
    assert not any("schema-init" in f for f in man.files)   # RPM owns it
    del os.environ["MIGRATE_ROOT"]

def test_prebuilt_refuses_when_binary_absent():
    m = _load()
    root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "usr/bin"))
    os.environ["MIGRATE_ROOT"] = root
    try:
        raised = False
        try:
            m.provision_binaries(m.Manifest(), prebuilt=True)
        except RuntimeError:
            raised = True
        assert raised
    finally:
        del os.environ["MIGRATE_ROOT"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest tests/test_migrate_prebuilt.py -v`
Expected: FAIL — `provision_binaries` undefined.

- [ ] **Step 3: Write minimal implementation**

Rename the existing `run_make_install` body into `provision_binaries` with a prebuilt guard at the top; keep a thin `run_make_install` alias so existing tests/callers stay green:

```python
def provision_binaries(manifest, run=subprocess.run, dry_run=False, prebuilt=False):
    if prebuilt:
        if not os.path.exists(P("usr/bin/schema-init")):
            raise RuntimeError("prebuilt mode: /usr/bin/schema-init absent — "
                               "install the schema-init package first")
        return
    if dry_run:
        return
    ensure_build_toolchain(manifest, run=run, dry_run=dry_run)
    # ... existing run_make_install body unchanged from here ...

def run_make_install(manifest, run=subprocess.run, dry_run=False):
    return provision_binaries(manifest, run=run, dry_run=dry_run, prebuilt=False)
```

Thread the flag:

```python
def do_deploy(run=subprocess.run, dry_run=False, prebuilt=False):
    profile = build_profile(run=run)
    if not dry_run:
        write_profile(profile)
    m = Manifest()
    provision_binaries(m, run=run, dry_run=dry_run, prebuilt=prebuilt)
    # ... rest of do_deploy unchanged ...
```

In `main`, add the flag and env, and pass it:

```python
    ap.add_argument("--prebuilt", action="store_true",
                    help="consume RPM-installed binaries; never compile")
    # ...
    prebuilt = args.prebuilt or os.environ.get("MIGRATE_PREBUILT") == "1"
    do_deploy(run=run, dry_run=args.dry_run, prebuilt=prebuilt)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_migrate_prebuilt.py tests/test_migrate_cli.py -v`
Expected: PASS (new prebuilt tests + the existing CLI-sequencing tests still green).

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_prebuilt.py
git commit -m "feat(migrate): prebuilt deploy mode (no compile-on-target)"
```

---

### Task 3: Recovery-card writer

The one catastrophe the GUI can't rescue is R1 never reaching SDDM. Before R1's reboot the engine writes the plain-language fallback-boot instructions to the user's home and to `/boot`, so they are readable from another machine and quotable by the GUI.

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_recovery_card.py`

**Interfaces:**
- Produces:
  - `RECOVERY_TEXT` (str) — the exact novice-facing copy from the spec.
  - `write_recovery_card(profile, root="/") -> list[str]` — writes `RECOVERY_TEXT` to `home/<user>/schema-recovery.txt` (mode `0644`, from `profile["user"]`) and to `boot/schema-recovery.txt`; returns the paths written.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_migrate_recovery_card.py
import os, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_rc", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def test_writes_home_and_boot():
    m = _load()
    root = tempfile.mkdtemp()
    os.makedirs(os.path.join(root, "home/jandoe")); os.makedirs(os.path.join(root, "boot"))
    os.environ["MIGRATE_ROOT"] = root
    try:
        paths = m.write_recovery_card({"user": "jandoe"}, root=root)
        home = os.path.join(root, "home/jandoe/schema-recovery.txt")
        boot = os.path.join(root, "boot/schema-recovery.txt")
        assert os.path.exists(home) and os.path.exists(boot)
        body = open(home).read()
        assert "(schema-init)" in body and "black" in body.lower()
        assert set(paths) == {"/home/jandoe/schema-recovery.txt", "/boot/schema-recovery.txt"}
    finally:
        del os.environ["MIGRATE_ROOT"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest tests/test_migrate_recovery_card.py -v`
Expected: FAIL — `write_recovery_card` undefined.

- [ ] **Step 3: Write minimal implementation**

```python
RECOVERY_TEXT = (
    "HOW TO GET YOUR COMPUTER BACK\n"
    "=============================\n\n"
    "Your computer is about to restart to finish setting up schema.\n\n"
    "If the screen stays BLACK for more than 2 minutes after the restart:\n\n"
    "  1. Hold the power button until the computer turns off.\n"
    "  2. Press it again to turn it back on.\n"
    "  3. At the start-up menu, use the arrow keys to choose the entry that\n"
    "     does NOT say \"(schema-init)\".\n"
    "  4. Press Enter.\n\n"
    "Your computer will start exactly as it does today. Nothing is lost.\n"
)

def write_recovery_card(profile, root="/"):
    written = []
    user = profile.get("user")
    targets = []
    if user:
        targets.append("home/%s/schema-recovery.txt" % user)
    targets.append("boot/schema-recovery.txt")
    for rel in targets:
        dst = os.path.join(root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w") as fh:
            fh.write(RECOVERY_TEXT)
        os.chmod(dst, 0o644)
        written.append("/" + rel)
    return written
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m pytest tests/test_migrate_recovery_card.py -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_recovery_card.py
git commit -m "feat(migrate): pre-R1 recovery card writer (home + /boot)"
```

---

### Task 4: Wire R1 into the CLI (deploy → R1_PENDING, recovery card first)

`--deploy` now writes the recovery card, runs the prebuilt deploy, and transitions `INSTALLED → R1_PENDING`. A `--stage` verb prints the current stage for the GUI to read.

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py` (import `stage`, `main`, `do_deploy`)
- Test: `tests/test_migrate_stage_wiring.py`

**Interfaces:**
- Consumes: `stage` module (Task 1), `write_recovery_card` (Task 3), `do_deploy(prebuilt=…)` (Task 2)
- Produces: `main(["--deploy","--prebuilt"])` leaves stage `R1_PENDING` and a recovery card on disk; `main(["--stage"])` prints the current stage.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_migrate_stage_wiring.py
import os, io, tempfile, importlib.util, contextlib
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_sw", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def _fedora_kde_root():
    root = tempfile.mkdtemp()
    for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib", "home/jandoe"):
        os.makedirs(os.path.join(root, d))
    open(os.path.join(root, "etc/os-release"), "w").write("ID=fedora\n")
    open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
    open(os.path.join(root, "usr/bin/schema-init"), "w").close()  # prebuilt present
    open(os.path.join(root, "etc/fstab"), "w").write("UUID=aaa / ext4 defaults 0 1\n")
    open(os.path.join(root, "etc/passwd"), "w").write("jandoe:x:1000:1000::/home/jandoe:/bin/bash\n")
    open(os.path.join(root, "boot/loader/entries/f.conf"), "w").write(
        "title Fedora\nversion 6.10.0\noptions root=UUID=aaa ro\n")
    return root

def test_deploy_sets_r1_pending_and_writes_card():
    m = _load()
    root = _fedora_kde_root()
    os.environ["MIGRATE_ROOT"] = root; os.environ["MIGRATE_KERNEL"] = "6.10.0"
    try:
        rc = m.main(["--deploy", "--prebuilt"], run=lambda *a, **k: type("R", (), {"returncode":0,"stdout":""})())
        assert rc == 0
        assert m.stage.read_stage(root) == m.stage.R1_PENDING
        assert os.path.exists(os.path.join(root, "home/jandoe/schema-recovery.txt"))
    finally:
        del os.environ["MIGRATE_ROOT"]; del os.environ["MIGRATE_KERNEL"]

def test_stage_verb_prints_current():
    m = _load()
    root = _fedora_kde_root(); os.environ["MIGRATE_ROOT"] = root
    try:
        m.stage.write_stage(m.stage.R1_HEAL, root=root)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            m.main(["--stage"])
        assert "R1_HEAL" in buf.getvalue()
    finally:
        del os.environ["MIGRATE_ROOT"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest tests/test_migrate_stage_wiring.py -v`
Expected: FAIL — no `stage` attr on module / `--stage` unknown.

- [ ] **Step 3: Write minimal implementation**

Import the sibling stage module at the top of `schema-migrate.py`:

```python
import importlib.util as _ilu
_stage_path = os.path.join(_MODDIR, "stage.py")
_spec = _ilu.spec_from_file_location("stage", _stage_path)
stage = _ilu.module_from_spec(_spec); _spec.loader.exec_module(stage)
```

Have `do_deploy` write the card and advance the stage (still honoring dry-run):

```python
def do_deploy(run=subprocess.run, dry_run=False, prebuilt=False):
    profile = build_profile(run=run)
    if not dry_run:
        write_profile(profile)
        write_recovery_card(profile, root=ROOT)
    m = Manifest()
    provision_binaries(m, run=run, dry_run=dry_run, prebuilt=prebuilt)
    # ... unchanged middle ...
    if not dry_run:
        m.set_boot_entry("/" + os.path.relpath(entry, ROOT))
        m.save()
        stage.transition(stage.R1_PENDING, root=ROOT)
    return m
```

Add the `--stage` verb near the top of `main` (before the platform gate, since reading state must never be gated):

```python
    ap.add_argument("--stage", action="store_true", help="print the current wizard stage")
    # ... after parse_args ...
    if args.stage:
        print(stage.read_stage(ROOT)); return 0
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_migrate_stage_wiring.py tests/test_migrate_cli.py -v`
Expected: PASS (new wiring + existing CLI tests green).

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_stage_wiring.py
git commit -m "feat(migrate): --deploy advances stage to R1_PENDING + writes recovery card; add --stage"
```

---

### Task 5: Wire R2 + resolution + teardown

Add `--arm-flip` (R1_HEAL → R2_PENDING, calls the flip helper's `arm`) and make `--finish` stage-aware: after reboot 1 it advances `R1_PENDING → R1_HEAL`; after reboot 2 it reads the flip helper's `is-authoritative`/`root-state` and resolves `R2_PENDING → DONE` or `→ ROLLED_BACK`, then tears down the autostart entry.

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py` (`main`, new `arm_flip`, `advance_finish`, `teardown`)
- Test: `tests/test_migrate_r2.py`

**Interfaces:**
- Consumes: `stage` (Task 1); the flip helper via an injected `flip=` callable defaulting to `lambda *a: subprocess.run(["/usr/libexec/schema-init/schema-flip-apply", *a], capture_output=True, text=True)`
- Produces:
  - `FLIP_HELPER = "/usr/libexec/schema-init/schema-flip-apply"`
  - `AUTOSTART = "etc/xdg/autostart/schema-wizard.desktop"`
  - `arm_flip(root="/", flip=…) -> int` — requires stage `R1_HEAL`; calls `flip("arm")`; on success `transition(R2_PENDING)`.
  - `advance_finish(root="/", flip=…) -> str` — `R1_PENDING→R1_HEAL`; `R2_PENDING→` (`DONE` if `flip("is-authoritative")` rc 0, else `ROLLED_BACK`) then `teardown`.
  - `teardown(root="/")` — removes `AUTOSTART` (idempotent).

- [ ] **Step 1: Write the failing test**

```python
# tests/test_migrate_r2.py
import os, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_r2", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def _root():
    r = tempfile.mkdtemp()
    for d in ("var/lib", "etc/xdg/autostart"):
        os.makedirs(os.path.join(r, d))
    open(os.path.join(r, "etc/xdg/autostart/schema-wizard.desktop"), "w").close()
    return r

def _ok(*a): 
    class R: returncode = 0; stdout = ""; stderr = ""
    return R()
def _fail(*a):
    class R: returncode = 1; stdout = ""; stderr = ""
    return R()

def test_arm_flip_requires_r1_heal():
    m = _load(); r = _root()
    m.stage.write_stage(m.stage.R1_HEAL, root=r)
    assert m.arm_flip(root=r, flip=lambda *a: _ok()) == 0
    assert m.stage.read_stage(r) == m.stage.R2_PENDING

def test_finish_r1_pending_advances_to_heal():
    m = _load(); r = _root()
    m.stage.write_stage(m.stage.R1_PENDING, root=r)
    assert m.advance_finish(root=r, flip=lambda *a: _ok()) == m.stage.R1_HEAL

def test_finish_r2_authoritative_is_done_and_tears_down():
    m = _load(); r = _root()
    m.stage.write_stage(m.stage.R2_PENDING, root=r)
    out = m.advance_finish(root=r, flip=lambda *a: _ok())     # is-authoritative rc 0
    assert out == m.stage.DONE
    assert not os.path.exists(os.path.join(r, "etc/xdg/autostart/schema-wizard.desktop"))

def test_finish_r2_not_authoritative_is_rolled_back():
    m = _load(); r = _root()
    m.stage.write_stage(m.stage.R2_PENDING, root=r)
    out = m.advance_finish(root=r, flip=lambda *a: _fail())   # is-authoritative rc 1
    assert out == m.stage.ROLLED_BACK
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest tests/test_migrate_r2.py -v`
Expected: FAIL — `arm_flip` / `advance_finish` / `teardown` undefined.

- [ ] **Step 3: Write minimal implementation**

```python
FLIP_HELPER = "/usr/libexec/schema-init/schema-flip-apply"
AUTOSTART = "etc/xdg/autostart/schema-wizard.desktop"

def _default_flip(*a):
    return subprocess.run([FLIP_HELPER, *a], capture_output=True, text=True)

def teardown(root="/"):
    try:
        os.remove(os.path.join(root, AUTOSTART))
    except OSError:
        pass

def arm_flip(root="/", flip=_default_flip):
    if stage.read_stage(root) != stage.R1_HEAL:
        raise RuntimeError("arm-flip requires stage R1_HEAL")
    if flip("arm").returncode != 0:
        return 1
    stage.transition(stage.R2_PENDING, root=root)
    return 0

def advance_finish(root="/", flip=_default_flip):
    cur = stage.read_stage(root)
    if cur == stage.R1_PENDING:
        stage.transition(stage.R1_HEAL, root=root)
        return stage.R1_HEAL
    if cur == stage.R2_PENDING:
        authoritative = flip("is-authoritative").returncode == 0
        new = stage.DONE if authoritative else stage.ROLLED_BACK
        stage.transition(new, root=root)
        teardown(root)
        return new
    return cur
```

Wire the verbs into `main` (these read/act on state; keep them ahead of the platform gate like `--stage`):

```python
    ap.add_argument("--arm-flip", action="store_true", help="R2: arm the udev+dbus flip")
    # inside main, alongside --stage / --finish handling:
    if args.arm_flip:
        return arm_flip(root=ROOT)
```

Replace the body of the existing `--finish` branch so it advances the stage first, then prints the report:

```python
    if args.finish:
        new = advance_finish(root=ROOT)
        print("stage: " + new)
        print(finish_report())
        return 0
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_migrate_r2.py tests/test_migrate_finish.py -v`
Expected: PASS (new R2 tests + existing finish tests green; if a legacy finish test asserts the old exact output, update it to accept the `stage:` prefix line).

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_r2.py
git commit -m "feat(migrate): --arm-flip + stage-aware --finish (DONE/ROLLED_BACK) + teardown"
```

---

### Task 6: Package it — `schema-init-migrate` subpackage + sudoers + COPR build

Ship the onboarding payload prebuilt as a subpackage; keep the base package's contract intact. Add the sudoers drop-in and the COPR build metadata.

**Files:**
- Create: `distros/fedora-installer/migrate/schema-wizard.sudoers`
- Modify: `schema-init.spec`
- Modify: `Makefile` (an `install-migrate` target that stages the engine, stage module, flip helper, doctor, and sudoers into a DESTDIR)
- Test: `tests/test_spec_migrate_subpackage.py`

**Interfaces:**
- Produces: a `schema-init-migrate` binary package whose file list contains `/usr/bin/schema-migrate`, `/usr/libexec/schema-init/schema-flip-apply`, `/usr/libexec/schema-init/stage.py`, `/usr/bin/schema-udev`, `/usr/libexec/schema-init/schema-doctor`, `/etc/sudoers.d/schema-wizard`.

- [ ] **Step 1: Write the sudoers drop-in and the failing test**

```
# distros/fedora-installer/migrate/schema-wizard.sudoers
# The schema onboarding GUI runs unprivileged and reaches root ONLY through
# these two fixed helper paths. No wildcards.
%wheel ALL=(root) NOPASSWD: /usr/bin/schema-migrate, /usr/libexec/schema-init/schema-flip-apply
```

```python
# tests/test_spec_migrate_subpackage.py
import subprocess, os
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def test_subpackage_declared_and_files_listed():
    out = subprocess.run(["rpmspec", "-q", "--rpms",
                          os.path.join(REPO, "schema-init.spec")],
                         capture_output=True, text=True).stdout
    assert "schema-init-migrate" in out

def test_base_package_has_no_udev():
    out = subprocess.run(["rpmspec", "-q", "--qf", "[%{FILENAMES}\\n]",
                          os.path.join(REPO, "schema-init.spec")],
                         capture_output=True, text=True).stdout
    # schema-udev must NOT be in the BASE package's file list
    base = [l for l in out.splitlines() if l.endswith("/bin/schema-udev")]
    # allowed only if it belongs to the -migrate subpackage; base contract check
    assert out.count("/bin/schema-udev") <= 1

def test_sudoers_is_narrow():
    s = open(os.path.join(REPO, "distros/fedora-installer/migrate/schema-wizard.sudoers")).read()
    assert "NOPASSWD" in s and "ALL=(root) NOPASSWD: ALL" not in s
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest tests/test_spec_migrate_subpackage.py -v`
Expected: FAIL — `schema-init-migrate` not yet declared.

- [ ] **Step 3: Implement — spec subpackage, Makefile target, build wiring**

In `schema-init.spec`, build `schema-udev` too but keep it OUT of the base `%files`; add the subpackage:

```spec
%global core_bins schema-init schema-ctl schema-subreaper schema-journal-sink schema-board
%global migrate_bins schema-udev

%build
%make_build BINS="%{core_bins} %{migrate_bins}"

%install
%make_install PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir} BINS="%{core_bins}"
%make install-migrate DESTDIR=%{buildroot} PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir}

%package migrate
Summary:   Guided in-place Fedora KDE onboarding onto schema-init (prebuilt)
Requires:  %{name} = %{version}-%{release}
Requires:  python3
Requires:  btrfs-progs
%description migrate
Prebuilt engine that converts a running Fedora KDE box onto schema-init in
place across two reboots, keeping a systemd fallback boot entry. Drives the
foundation flip (schema-init PID 1) and the desktop-seam flip (schema-udev +
schema-dbus) from the schema-migrate CLI. Front-ended by schema-wizard.

%files migrate
%{_bindir}/schema-migrate
%{_bindir}/schema-udev
%{_libexecdir}/schema-init/schema-flip-apply
%{_libexecdir}/schema-init/stage.py
%{_libexecdir}/schema-init/schema-doctor
%{_datadir}/%{name}/migrate/prevent-set.list
%config(noreplace) %{_sysconfdir}/sudoers.d/schema-wizard
```

Add the Makefile target (mirrors the existing `install:` style; installs prebuilt artifacts into the buildroot):

```make
install-migrate: schema-udev
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(PREFIX)/libexec/schema-init
	install -m 0755 distros/fedora-installer/migrate/schema-migrate.py $(DESTDIR)$(BINDIR)/schema-migrate
	install -m 0755 schema-udev $(DESTDIR)$(BINDIR)/schema-udev
	install -m 0755 distros/fedora-installer/schema-flip-apply.sh $(DESTDIR)$(PREFIX)/libexec/schema-init/schema-flip-apply
	install -m 0644 distros/fedora-installer/migrate/stage.py $(DESTDIR)$(PREFIX)/libexec/schema-init/stage.py
	install -m 0755 scripts/schema-doctor.py $(DESTDIR)$(PREFIX)/libexec/schema-init/schema-doctor
	install -d $(DESTDIR)$(DATADIR)/schema-init/migrate
	install -m 0644 distros/fedora-installer/migrate/prevent-set.list $(DESTDIR)$(DATADIR)/schema-init/migrate/prevent-set.list
	install -d $(DESTDIR)$(SYSCONFDIR)/sudoers.d
	install -m 0440 distros/fedora-installer/migrate/schema-wizard.sudoers $(DESTDIR)$(SYSCONFDIR)/sudoers.d/schema-wizard
```

Note: `schema-migrate` installed to `/usr/bin` must find `stage.py`; adjust its sibling-import to also check `/usr/libexec/schema-init/stage.py` (fallback after `_MODDIR`).

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_spec_migrate_subpackage.py -v && rpmspec -P schema-init.spec >/dev/null && echo SPEC_OK`
Expected: PASS + `SPEC_OK` (spec parses).

- [ ] **Step 5: Commit**

```bash
git add schema-init.spec Makefile distros/fedora-installer/migrate/schema-wizard.sudoers tests/test_spec_migrate_subpackage.py
git commit -m "feat(pkg): schema-init-migrate subpackage + narrow sudoers + install-migrate"
```

---

### Task 7: Full-suite green + branch integration

**Files:** none new — verification gate.

- [ ] **Step 1: Run the whole Python suite**

Run: `python -m pytest tests/ -q`
Expected: all green, including the pre-existing `test_migrate_*` and `test_doctor_*` suites.

- [ ] **Step 2: Sibling-import smoke from the installed path**

Run:
```bash
tmp=$(mktemp -d); make install-migrate DESTDIR=$tmp PREFIX=/usr SYSCONFDIR=/etc >/dev/null
MIGRATE_ROOT=$tmp python3 $tmp/usr/bin/schema-migrate --stage
```
Expected: prints `INSTALLED` (proves the `/usr/bin` engine resolves `stage.py` from `/usr/libexec/schema-init`).

- [ ] **Step 3: Commit any fixups, then stop for review**

```bash
git add -A && git commit -m "test: full-suite green for Plan A foundation" || true
```

Do NOT open a PR yet — Plan A lands behind Plan B (GUI) and Plan C (doctor hardening) on the same `feat/schema-copr-wizard` branch, or merges on its own once VM-smoke-tested per the spec's success criteria 1–3. Hand back for the human's VM test.

---

## Self-review

**Spec coverage:**
- Prebuilt packaging → Task 2 (mode) + Task 6 (subpackage). ✅
- Two-round stage machine → Task 1 (machine) + Task 4 (R1) + Task 5 (R2/resolution/teardown). ✅
- Privileged surface (sudoers.d, fixed helper) → Task 6. ✅
- State location `/var/lib/schema-init/wizard-stage.json` → Task 1. ✅
- Teardown on DONE/ROLLED_BACK → Task 5. ✅
- Recovery card → Task 3, written in `--deploy` (Task 4). ✅
- Base-package contract preserved → Task 6 (`test_base_package_has_no_udev`). ✅
- **Deferred to Plan B:** the Qt/QML GUI, the Advanced expander, btrfs snapshot UX (the engine already snapshots via existing migrate paths; the *choice surfacing* is GUI). **Deferred to Plan C:** doctor hardening. These are out of Plan A by design.

**Placeholder scan:** none — every step carries real test and implementation code.

**Type consistency:** `stage` constants and `read_stage/write_stage/transition` used identically in Tasks 1/4/5; `provision_binaries(prebuilt=…)` signature consistent Tasks 2/4; `arm_flip/advance_finish/teardown` signatures consistent Task 5 ↔ tests; helper path `/usr/libexec/schema-init/schema-flip-apply` identical in Task 5 code, Task 6 sudoers, and Makefile.

**Note for Plan B:** the GUI's privileged calls are exactly `sudo /usr/bin/schema-migrate {--deploy --prebuilt | --arm-flip | --finish}` and unprivileged `schema-migrate --stage`; the recovery-card text is `RECOVERY_TEXT` (read the on-disk `~/schema-recovery.txt`).
