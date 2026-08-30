# schema-migrate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `schema-migrate` — a USB tool that transitions an existing Fedora KDE box onto schema-init/schema-udev in place, losing nothing, adds schema as a non-default fallback boot entry, and lets schema-doctor heal the seams on the reboot after.

**Architecture:** One stdlib-only Python module `distros/fedora-installer/migrate/schema-migrate.py` (mirrors `scripts/schema-doctor.py`: `MIGRATE_ROOT`-injectable, tests loaded via `importlib`), plus a data file `prevent-set.list` and a rail service. Three phases: **discover** (read-only → profile JSON), **deploy** (make install + curated prevent-set + generated host units + packages + BLS boot entry, all recorded to a reversible manifest), **finish** (post-reboot report + translate offer). Reuses `make install`, the `fedora-kde/` profile, and `schema-doctor`.

**Tech Stack:** Python 3 stdlib only. `subprocess` to `make`/`dnf`/`systemctl` behind an injectable `run` callable so unit tests never shell out. Fedora BLS boot entries. schema-init `.svc` format.

**Spec:** `docs/superpowers/specs/2026-08-29-schema-migrate-design.md`

## Global Constraints

- **Stdlib only.** No third-party Python. dbus/pkg/systemd reached via `subprocess`, always through an injectable `run` parameter (default `subprocess.run`) so tests inject a fake.
- **`MIGRATE_ROOT`** env (default `/`) prefixes every filesystem path. Helper `P(rel)` joins it. Tests set it to a temp tree. Exception: paths that come from a live process's own `HOME`/absolute value are used as-is (none in this tool — all paths are ROOT-relative).
- **`MIGRATE_REPO`** env points at the schema-init tree on the USB (the dir containing `distros/`, `Makefile`). Default: three levels up from the module file. Tests inject a fake repo tree.
- **Additive only.** The tool never deletes or overwrites an existing system file in place. Everything it writes is recorded to `/var/lib/schema-init/migrate-manifest.json`.
- **Fedora KDE only (v1).** Detect and refuse anything else. No systemd-unit translator (deferred). Schema is never made the default boot entry.
- **Install path:** `make install PREFIX=/usr` → binaries in `/usr/bin`, so the boot entry uses `init=/usr/bin/schema-init`.
- **Test convention (match the doctor):** each test is `tests/test_migrate_<area>.py`, self-contained, loads the module via `importlib.util.spec_from_file_location`, uses a local `check(name, ok)` helper, prints `PASS`/`FAIL`, `sys.exit(0 if all else 1)`. Runs directly (`python3 tests/test_migrate_x.py`), NOT in the C-only Makefile `test` target.
- **Commit trailer:** every commit ends with the two lines
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke`.
- **Branch:** `feat/schema-doctor-migration-checks` (continues) unless split.

## File Structure

- Create: `distros/fedora-installer/migrate/schema-migrate.py` — the whole tool (discover, deploy, bootentry, manifest, finish, CLI). Built up task by task.
- Create: `distros/fedora-installer/migrate/prevent-set.list` — the curated portable core (data).
- Create: `distros/fedora-installer/rail/services/schema-migrate-finish.svc` — post-reboot oneshot.
- Create: `tests/test_migrate_preventset.py`, `test_migrate_platform.py`, `test_migrate_services.py`, `test_migrate_profile.py`, `test_migrate_bootentry.py`, `test_migrate_manifest.py`, `test_migrate_uninstall.py`, `test_migrate_deploy_profile.py`, `test_migrate_hostunits.py`, `test_migrate_packages.py`, `test_migrate_cli.py`, `test_migrate_finish.py`.

Module section order (all in `schema-migrate.py`): imports + `ROOT`/`P`/`REPO` helpers → prevent-set parser → platform detect → service enumeration/classification → profile builder → boot entry → manifest → deploy helpers → finish → `main()`.

---

### Task 1: Module skeleton + prevent-set list & parser

**Files:**
- Create: `distros/fedora-installer/migrate/schema-migrate.py`
- Create: `distros/fedora-installer/migrate/prevent-set.list`
- Test: `tests/test_migrate_preventset.py`

**Interfaces:**
- Produces: `ROOT`, `def P(rel)`, `def repo()`, `def load_prevent_set(path=None) -> dict` returning `{"script":[...], "config":[...], "service":[...], "exclude":[...]}`.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""prevent-set.list parser tests."""
import os, sys, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

ps = sm.load_prevent_set()
check("returns the four categories",
      set(ps) == {"script", "config", "service", "exclude"})
check("scripts include plasma-session-start.sh", "plasma-session-start.sh" in ps["script"])
check("services include schema-logind", "schema-logind" in ps["service"])
check("config includes plasma-workspace", any("plasma-workspace" in c for c in ps["config"]))
check("frigate is excluded, not a service",
      "frigate" in ps["exclude"] and "frigate" not in ps["service"])
check("comments and blanks ignored", "" not in ps["service"] and "#" not in "".join(ps["service"]))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_preventset.py`
Expected: FAIL — module file / `load_prevent_set` does not exist.

- [ ] **Step 3: Write the data file `prevent-set.list`**

```
# schema-migrate prevent-set — the portable desktop-seam core of the
# fedora-kde profile. Deploy copies exactly what is named here.
# Format: "<category> <name>"; categories: script config service exclude.

# --- scripts (from distros/fedora-kde/scripts/) ---
script plasma-session-start.sh
script schema-plasma-autologin.sh
script schema-plasma-watchdog.sh
script schema-autostart-runner.sh
script schema-logind.py
script seatd-run.sh
script pipewire-run.sh
script wireplumber-run.sh
script pipewire-pulse-run.sh

# --- config (from distros/fedora-kde/config/) ---
config plasma-env
config plasma-workspace
config polkit
config autostart

# --- services (from distros/fedora-installer/rail/services/) ---
service dbus
service seatd
service schema-logind
service plasma-autologin
service polkitd
service bluetoothd
service network-manager
service sshd
service zram
service schema-doctor
service schema-doctor-periodic

# --- excluded: host-specific, never deployed to a stranger ---
exclude frigate
exclude greybox-audio
exclude nordvpnd
exclude ollama
exclude mount-home
exclude network-blakbox
exclude loop-module
```

- [ ] **Step 4: Write the module skeleton + parser**

```python
#!/usr/bin/env python3
"""schema-migrate — transition an existing Fedora KDE box onto schema-init
in place, non-destructively, with a fallback boot entry and post-reboot heal.

Stdlib only. MIGRATE_ROOT prefixes filesystem paths (tests inject a temp tree);
MIGRATE_REPO points at the schema-init tree on the USB.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.environ.get("MIGRATE_ROOT") or "/"
_MODDIR = os.path.dirname(os.path.abspath(__file__))


def P(rel):
    return os.path.join(ROOT, rel.lstrip("/"))


def repo():
    return os.environ.get("MIGRATE_REPO") or os.path.dirname(os.path.dirname(os.path.dirname(_MODDIR)))


def load_prevent_set(path=None):
    if path is None:
        path = os.path.join(_MODDIR, "prevent-set.list")
    out = {"script": [], "config": [], "service": [], "exclude": []}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cat, _, name = line.partition(" ")
            name = name.strip()
            if cat in out and name:
                out[cat].append(name)
    return out
```

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 tests/test_migrate_preventset.py`
Expected: PASS (all 6 checks).

- [ ] **Step 6: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py \
        distros/fedora-installer/migrate/prevent-set.list \
        tests/test_migrate_preventset.py
git commit -m "schema-migrate: module skeleton + curated prevent-set list

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 2: Platform detection (Fedora KDE, refuse otherwise)

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_platform.py`

**Interfaces:**
- Consumes: `P`.
- Produces: `def detect_platform() -> tuple[str|None, str]` returning `(("fedora","kde") joined as "fedora-kde", "")` on success, or `(None, reason)` on refusal. Reads `P("etc/os-release")` for `ID=fedora`; KDE proven by `P("usr/bin/plasmashell")` existing OR `P("usr/bin/sddm")` existing.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""platform detection tests — fake os-release + KDE markers under MIGRATE_ROOT."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
os.makedirs(os.path.join(root, "etc")); os.makedirs(os.path.join(root, "usr/bin"))
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# no os-release → refuse
plat, why = sm.detect_platform()
check("refuse when os-release missing", plat is None and "fedora" in why.lower())

# fedora but no KDE marker → refuse
open(os.path.join(root, "etc/os-release"), "w").write('ID=fedora\nVERSION_ID=44\n')
plat, why = sm.detect_platform()
check("refuse fedora without KDE", plat is None and "kde" in why.lower())

# not fedora → refuse
open(os.path.join(root, "etc/os-release"), "w").write('ID=debian\n')
open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
plat, why = sm.detect_platform()
check("refuse non-fedora", plat is None)

# fedora + plasmashell → accept
open(os.path.join(root, "etc/os-release"), "w").write('ID=fedora\nVERSION_ID=44\n')
plat, why = sm.detect_platform()
check("accept fedora kde", plat == "fedora-kde")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_platform.py`
Expected: FAIL — `detect_platform` not defined.

- [ ] **Step 3: Implement**

```python
def _os_release():
    kv = {}
    try:
        for line in open(P("etc/os-release")):
            k, _, v = line.strip().partition("=")
            kv[k] = v.strip().strip('"')
    except OSError:
        pass
    return kv


def detect_platform():
    osr = _os_release()
    if osr.get("ID") != "fedora":
        return None, "this box is not Fedora (os-release ID=%s) — v1 supports Fedora only" % osr.get("ID", "unknown")
    if not (os.path.exists(P("usr/bin/plasmashell")) or os.path.exists(P("usr/bin/sddm"))):
        return None, "no KDE found (plasmashell/sddm absent) — v1 supports Fedora KDE only"
    return "fedora-kde", ""
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_platform.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_platform.py
git commit -m "schema-migrate: platform detection (Fedora KDE, refuse otherwise)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 3: Running-service enumeration + classification

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_services.py`

**Interfaces:**
- Consumes: `load_prevent_set`.
- Produces:
  - constants `SCHEMA_OWNED` (systemd unit basenames schema replaces) and `UNIT_TO_SVC` (systemd unit basename → prevent-set service name).
  - `def running_services(run=subprocess.run) -> list[str]` — basenames (no `.service`) of running units via `systemctl list-units --type=service --state=running --no-legend --plain`.
  - `def classify_services(units, prevent) -> dict` → `{"covered":[...], "schema_owned":[...], "leftover":[...]}` (each sorted, unique).

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""service classification tests — no systemctl, inject the unit list."""
import os, sys, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# fake systemctl output through an injected runner
class R:
    def __init__(self, out): self.stdout = out; self.returncode = 0
def fake_run(argv, **kw):
    return R("NetworkManager.service loaded active running Network Manager\n"
             "sshd.service loaded active running OpenSSH\n"
             "systemd-udevd.service loaded active running udev\n"
             "tailscaled.service loaded active running Tailscale\n")

units = sm.running_services(run=fake_run)
check("parses unit basenames", "NetworkManager" in units and "tailscaled" in units)

c = sm.classify_services(units, sm.load_prevent_set())
check("NetworkManager is covered", "network-manager" in c["covered"])
check("sshd is covered", "sshd" in c["covered"])
check("systemd-udevd is schema-owned", "systemd-udevd" in c["schema_owned"])
check("tailscaled is leftover", "tailscaled" in c["leftover"])
check("no unit lands in two buckets",
      not (set(c["covered"]) & set(c["leftover"])) and not (set(c["schema_owned"]) & set(c["leftover"])))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_services.py`
Expected: FAIL — `running_services` not defined.

- [ ] **Step 3: Implement**

```python
SCHEMA_OWNED = {
    "systemd-udevd", "systemd-journald", "systemd-logind", "systemd-resolved",
    "crond", "cron", "systemd-timesyncd", "systemd-userdbd",
}

UNIT_TO_SVC = {
    "NetworkManager": "network-manager",
    "sshd": "sshd",
    "bluetooth": "bluetoothd",
    "polkit": "polkitd",
    "dbus": "dbus",
    "dbus-broker": "dbus",
    "seatd": "seatd",
}


def running_services(run=subprocess.run):
    try:
        r = run(["systemctl", "list-units", "--type=service", "--state=running",
                 "--no-legend", "--plain"], capture_output=True, text=True)
        out = r.stdout
    except Exception:
        out = ""
    units = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        unit = line.split()[0]
        if unit.endswith(".service"):
            units.append(unit[:-len(".service")])
    return units


def classify_services(units, prevent):
    covered, owned, leftover = set(), set(), set()
    svc_set = set(prevent["service"])
    for u in units:
        if u in SCHEMA_OWNED:
            owned.add(u)
        elif u in UNIT_TO_SVC and UNIT_TO_SVC[u] in svc_set:
            covered.add(UNIT_TO_SVC[u])
        else:
            leftover.add(u)
    return {"covered": sorted(covered), "schema_owned": sorted(owned),
            "leftover": sorted(leftover)}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_services.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_services.py
git commit -m "schema-migrate: enumerate + classify running services

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 4: Mounts, primary user, bootloader → full profile + JSON

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_profile.py`

**Interfaces:**
- Consumes: `P`, `detect_platform`, `running_services`, `classify_services`, `load_prevent_set`.
- Produces:
  - `SKIP_FSTYPES` (pseudo-fs to skip).
  - `def read_fstab() -> list[dict]` from `P("etc/fstab")`: each `{"src","target","fstype","opts"}`, skipping comments and `SKIP_FSTYPES`.
  - `def primary_user() -> tuple[str|None,int|None]` — first `P("etc/passwd")` entry with uid 1000.
  - `def bootloader_kind() -> str` — `"bls"` if `P("boot/loader/entries")` has `*.conf`, else `"unknown"`.
  - `def build_profile(run=subprocess.run) -> dict` — assembles `{"platform","user","uid","services","mounts","bootloader","kernel"}` (kernel from `os.uname().release`, overridable via `MIGRATE_KERNEL`).
  - `def write_profile(profile) -> str` — writes `P("var/lib/schema-init/migrate-profile.json")` (0644), returns path.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""profile assembly tests — fake fstab/passwd/os-release/BLS under MIGRATE_ROOT."""
import os, sys, json, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
os.environ["MIGRATE_KERNEL"] = "6.10.0-test.fc44.x86_64"
for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "etc/os-release"), "w").write('ID=fedora\nVERSION_ID=44\n')
open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
open(os.path.join(root, "etc/fstab"), "w").write(
    "# comment\nUUID=aaa / ext4 defaults 0 1\n"
    "UUID=bbb /home xfs defaults 0 2\nproc /proc proc defaults 0 0\n")
open(os.path.join(root, "etc/passwd"), "w").write(
    "root:x:0:0:root:/root:/bin/bash\njandoe:x:1000:1000:Jan:/home/jandoe:/bin/bash\n")
open(os.path.join(root, "boot/loader/entries/x.conf"), "w").write("title Fedora\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

m = sm.read_fstab()
check("fstab skips comments + pseudo-fs", len(m) == 2 and all(x["fstype"] != "proc" for x in m))
check("fstab captures /home xfs", any(x["target"] == "/home" and x["fstype"] == "xfs" for x in m))
u, uid = sm.primary_user()
check("primary user is uid 1000", u == "jandoe" and uid == 1000)
check("bootloader detected as bls", sm.bootloader_kind() == "bls")

prof = sm.build_profile(run=lambda *a, **k: type("R", (), {"stdout": "", "returncode": 0})())
check("profile carries platform", prof["platform"] == "fedora-kde")
check("profile carries kernel from env", prof["kernel"] == "6.10.0-test.fc44.x86_64")
check("profile has mounts + user + services keys",
      "mounts" in prof and prof["user"] == "jandoe" and "services" in prof)

path = sm.write_profile(prof)
check("profile json written", os.path.exists(path) and json.load(open(path))["platform"] == "fedora-kde")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_profile.py`
Expected: FAIL — `read_fstab` not defined.

- [ ] **Step 3: Implement**

```python
SKIP_FSTYPES = {"proc", "sysfs", "devpts", "tmpfs", "devtmpfs", "cgroup",
                "cgroup2", "mqueue", "hugetlbfs", "debugfs", "swap", "efivarfs"}


def read_fstab():
    out = []
    try:
        lines = open(P("etc/fstab")).read().splitlines()
    except OSError:
        return out
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = line.split()
        if len(f) < 3 or f[2] in SKIP_FSTYPES:
            continue
        out.append({"src": f[0], "target": f[1], "fstype": f[2],
                    "opts": f[3] if len(f) > 3 else "defaults"})
    return out


def primary_user():
    try:
        for line in open(P("etc/passwd")):
            f = line.split(":")
            if len(f) >= 3 and f[2] == "1000":
                return f[0], 1000
    except OSError:
        pass
    return None, None


def bootloader_kind():
    import glob
    if glob.glob(P("boot/loader/entries/*.conf")):
        return "bls"
    return "unknown"


def build_profile(run=subprocess.run):
    plat, _ = detect_platform()
    user, uid = primary_user()
    prevent = load_prevent_set()
    services = classify_services(running_services(run=run), prevent)
    return {
        "platform": plat,
        "user": user,
        "uid": uid,
        "services": services,
        "mounts": read_fstab(),
        "bootloader": bootloader_kind(),
        "kernel": os.environ.get("MIGRATE_KERNEL") or os.uname().release,
    }


def write_profile(profile):
    p = P("var/lib/schema-init/migrate-profile.json")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as fh:
        json.dump(profile, fh, indent=2)
    os.chmod(p, 0o644)
    return p
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_profile.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_profile.py
git commit -m "schema-migrate: mounts/user/bootloader -> profile json (discover complete)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 5: BLS boot entry — clone active, add/remove

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_bootentry.py`

**Interfaces:**
- Consumes: `P`.
- Produces:
  - `SCHEMA_ENTRY_ID = "schema-init"` and `INIT_PATH = "/usr/bin/schema-init"`.
  - `def _active_entry(kernel) -> str|None` — path of the BLS `.conf` whose `version`/filename contains `kernel`, else the lexically-last `.conf`.
  - `def add_boot_entry(kernel) -> str` — clone the active entry to `P("boot/loader/entries/%s.conf" % SCHEMA_ENTRY_ID)`, set `title "<orig> (schema-init)"`, append ` init=/usr/bin/schema-init` to the `options` line (idempotent — never doubled), return the new file path. Never touches any other entry or the default.
  - `def remove_boot_entry() -> None` — delete the schema entry file if present.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""BLS boot-entry tests — fake /boot/loader/entries under MIGRATE_ROOT."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
ENTRIES = os.path.join(root, "boot/loader/entries"); os.makedirs(ENTRIES)
open(os.path.join(ENTRIES, "fedora-6.10.0.conf"), "w").write(
    "title Fedora Linux 44\nversion 6.10.0\nlinux /vmlinuz-6.10.0\n"
    "initrd /initramfs-6.10.0.img\noptions root=UUID=aaa ro quiet\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

path = sm.add_boot_entry("6.10.0")
body = open(path).read()
check("schema entry created", path.endswith("schema-init.conf") and os.path.exists(path))
check("title marks schema-init", "(schema-init)" in body)
check("options carry init=/usr/bin/schema-init", "init=/usr/bin/schema-init" in body)
check("keeps original kernel/root", "root=UUID=aaa" in body and "/vmlinuz-6.10.0" in body)
orig = open(os.path.join(ENTRIES, "fedora-6.10.0.conf")).read()
check("original entry untouched", "(schema-init)" not in orig and "init=/usr/bin/schema-init" not in orig)

sm.add_boot_entry("6.10.0")  # idempotent
check("init= not doubled on re-add", open(path).read().count("init=/usr/bin/schema-init") == 1)

sm.remove_boot_entry()
check("remove deletes schema entry", not os.path.exists(path))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_bootentry.py`
Expected: FAIL — `add_boot_entry` not defined.

- [ ] **Step 3: Implement**

```python
import glob as _glob

SCHEMA_ENTRY_ID = "schema-init"
INIT_PATH = "/usr/bin/schema-init"


def _active_entry(kernel):
    entries = sorted(_glob.glob(P("boot/loader/entries/*.conf")))
    entries = [e for e in entries if os.path.basename(e) != SCHEMA_ENTRY_ID + ".conf"]
    if not entries:
        return None
    for e in entries:
        if kernel and kernel in os.path.basename(e):
            return e
    for e in entries:
        if kernel and kernel in open(e).read():
            return e
    return entries[-1]


def add_boot_entry(kernel):
    src = _active_entry(kernel)
    dst = P("boot/loader/entries/%s.conf" % SCHEMA_ENTRY_ID)
    lines = open(src).read().splitlines() if src else ["options ro"]
    out = []
    for line in lines:
        if line.startswith("title "):
            out.append(line.rstrip() + " (schema-init)")
        elif line.startswith("options "):
            if ("init=" + INIT_PATH) not in line:
                line = line.rstrip() + " init=" + INIT_PATH
            out.append(line)
        else:
            out.append(line)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w") as fh:
        fh.write("\n".join(out) + "\n")
    return dst


def remove_boot_entry():
    dst = P("boot/loader/entries/%s.conf" % SCHEMA_ENTRY_ID)
    try:
        os.remove(dst)
    except OSError:
        pass
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_bootentry.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_bootentry.py
git commit -m "schema-migrate: BLS boot entry clone (fallback, default untouched)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 6: Manifest — record + save/load

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_manifest.py`

**Interfaces:**
- Consumes: `P`.
- Produces: class `Manifest` with `.files:list`, `.packages:list`, `.boot_entry:str|None`; methods `add_file(path)`, `add_package(name)`, `set_boot_entry(path)`, `save()` (→ `P("var/lib/schema-init/migrate-manifest.json")`, 0644), classmethod `load()` (empty manifest if absent/corrupt), and `PATH` constant.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""manifest record/save/load tests."""
import os, sys, json, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

m = sm.Manifest()
m.add_file("/usr/bin/schema-init"); m.add_file("/usr/bin/schema-init")  # dedup
m.add_package("libavcodec-freeworld")
m.set_boot_entry("/boot/loader/entries/schema-init.conf")
m.save()

check("dedups files", m.files.count("/usr/bin/schema-init") == 1)
m2 = sm.Manifest.load()
check("reloads files", "/usr/bin/schema-init" in m2.files)
check("reloads packages", "libavcodec-freeworld" in m2.packages)
check("reloads boot entry", m2.boot_entry.endswith("schema-init.conf"))

# corrupt file → empty manifest, no crash
open(os.path.join(root, sm.Manifest.PATH), "w").write("{ not json")
m3 = sm.Manifest.load()
check("corrupt manifest loads empty", m3.files == [] and m3.boot_entry is None)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_manifest.py`
Expected: FAIL — `Manifest` not defined.

- [ ] **Step 3: Implement**

```python
class Manifest:
    PATH = "var/lib/schema-init/migrate-manifest.json"

    def __init__(self, files=None, packages=None, boot_entry=None):
        self.files = list(files or [])
        self.packages = list(packages or [])
        self.boot_entry = boot_entry

    def add_file(self, path):
        if path not in self.files:
            self.files.append(path)

    def add_package(self, name):
        if name not in self.packages:
            self.packages.append(name)

    def set_boot_entry(self, path):
        self.boot_entry = path

    def save(self):
        p = P(self.PATH)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as fh:
            json.dump({"files": self.files, "packages": self.packages,
                       "boot_entry": self.boot_entry}, fh, indent=2)
        os.chmod(p, 0o644)
        return p

    @classmethod
    def load(cls):
        try:
            d = json.load(open(P(cls.PATH)))
            if not isinstance(d, dict):
                d = {}
        except (OSError, ValueError):
            d = {}
        return cls(d.get("files"), d.get("packages"), d.get("boot_entry"))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_manifest.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_manifest.py
git commit -m "schema-migrate: reversible manifest (files/packages/boot entry)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 7: Uninstall — reverse the manifest

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_uninstall.py`

**Interfaces:**
- Consumes: `Manifest`, `P`, `remove_boot_entry`.
- Produces: `def uninstall(run=subprocess.run) -> dict` — loads the manifest, removes each recorded file (ignore missing), `dnf remove -y` the recorded packages (via `run`; only those recorded, which by construction were newly installed), removes the boot entry, deletes the manifest. Returns `{"files_removed":n, "packages":[...]}`. Never removes a path not in the manifest.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""uninstall reverses exactly the manifest — no live dnf (inject runner)."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
os.makedirs(os.path.join(root, "usr/bin")); os.makedirs(os.path.join(root, "boot/loader/entries"))
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

added = os.path.join(root, "usr/bin/schema-init"); open(added, "w").close()
untracked = os.path.join(root, "usr/bin/keepme"); open(untracked, "w").close()
entry = os.path.join(root, "boot/loader/entries/schema-init.conf"); open(entry, "w").close()

m = sm.Manifest(); m.add_file("/usr/bin/schema-init"); m.add_package("libavcodec-freeworld")
m.set_boot_entry("/boot/loader/entries/schema-init.conf"); m.save()

calls = []
def fake_run(argv, **kw):
    calls.append(argv); return type("R", (), {"returncode": 0, "stdout": ""})()

res = sm.uninstall(run=fake_run)
check("removed the tracked file", not os.path.exists(added))
check("left the untracked file", os.path.exists(untracked))
check("removed the boot entry", not os.path.exists(entry))
check("dnf remove called for the package",
      any("remove" in c and "libavcodec-freeworld" in c for c in calls))
check("manifest deleted", not os.path.exists(os.path.join(root, sm.Manifest.PATH)))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_uninstall.py`
Expected: FAIL — `uninstall` not defined.

- [ ] **Step 3: Implement**

```python
def uninstall(run=subprocess.run):
    m = Manifest.load()
    removed = 0
    for rel in m.files:
        try:
            os.remove(P(rel))
            removed += 1
        except OSError:
            pass
    if m.packages:
        run(["dnf", "remove", "-y"] + m.packages, check=False)
    remove_boot_entry()
    try:
        os.remove(P(Manifest.PATH))
    except OSError:
        pass
    return {"files_removed": removed, "packages": m.packages}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_uninstall.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_uninstall.py
git commit -m "schema-migrate: manifest-driven uninstall (reverse exactly what deploy added)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 8: Deploy the prevent-set (curated copy)

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_deploy_profile.py`

**Interfaces:**
- Consumes: `load_prevent_set`, `repo`, `P`, `Manifest`.
- Produces: `def deploy_prevent_set(manifest, dry_run=False) -> list[str]` — copies each named script → `P("usr/local/lib/schema-init/scripts/<name>")`, each config dir → `P("etc/schema-init/config/<name>")` (recursive), each service `<name>.svc` (from `distros/fedora-installer/rail/services/`, falling back to `distros/fedora-kde/services/`) → `P("etc/schema-init/services/<name>.svc")`. Records every destination in the manifest. Excluded names are never copied (they are not in the copy categories). Returns the list of destination paths. `dry_run` computes/returns the list without writing.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""prevent-set copy tests — fake repo tree + fake prevent-set."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
fakerepo = tempfile.mkdtemp(); os.environ["MIGRATE_REPO"] = fakerepo
# minimal fake repo layout
for d in ("distros/fedora-kde/scripts", "distros/fedora-kde/config/plasma-env",
          "distros/fedora-installer/rail/services", "distros/fedora-installer/migrate"):
    os.makedirs(os.path.join(fakerepo, d))
open(os.path.join(fakerepo, "distros/fedora-kde/scripts/plasma-session-start.sh"), "w").write("#!/bin/sh\n")
open(os.path.join(fakerepo, "distros/fedora-kde/config/plasma-env/env.sh"), "w").write("x=1\n")
open(os.path.join(fakerepo, "distros/fedora-installer/rail/services/dbus.svc"), "w").write("name=dbus\n")
# a tiny prevent-set naming just those + an excluded item
psl = os.path.join(fakerepo, "distros/fedora-installer/migrate/prevent-set.list")
open(psl, "w").write("script plasma-session-start.sh\nconfig plasma-env\nservice dbus\nexclude frigate\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
# point the module's prevent-set loader at the fake list
sm._PREVENT_LIST_OVERRIDE = psl
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

m = sm.Manifest()
dst = sm.deploy_prevent_set(m)
check("copied the script", os.path.exists(os.path.join(root, "usr/local/lib/schema-init/scripts/plasma-session-start.sh")))
check("copied the config dir", os.path.exists(os.path.join(root, "etc/schema-init/config/plasma-env/env.sh")))
check("copied the service", os.path.exists(os.path.join(root, "etc/schema-init/services/dbus.svc")))
check("nothing frigate copied", not any("frigate" in p for p in dst))
check("manifest recorded the copies", any("plasma-session-start.sh" in f for f in m.files))

# dry-run writes nothing
root2 = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root2
import importlib; importlib.reload(sm); sm._PREVENT_LIST_OVERRIDE = psl
m2 = sm.Manifest(); plan = sm.deploy_prevent_set(m2, dry_run=True)
check("dry-run returns a plan", len(plan) >= 3)
check("dry-run wrote nothing", not os.path.exists(os.path.join(root2, "etc/schema-init")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_deploy_profile.py`
Expected: FAIL — `deploy_prevent_set` not defined.

- [ ] **Step 3: Implement**

(Add an override hook to `load_prevent_set` so a test list can be injected, then the copier.)

```python
_PREVENT_LIST_OVERRIDE = None  # tests may set this to a path
```

Change the top of `load_prevent_set` to honor it:

```python
def load_prevent_set(path=None):
    if path is None:
        path = _PREVENT_LIST_OVERRIDE or os.path.join(_MODDIR, "prevent-set.list")
    # ... unchanged body ...
```

Then:

```python
def _copy_into(src, dst, manifest, dry_run):
    if dry_run:
        return dst
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.isdir(src):
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        shutil.copy2(src, dst)
    return dst


def deploy_prevent_set(manifest, dry_run=False):
    ps = load_prevent_set()
    r = repo()
    written = []
    for name in ps["script"]:
        src = os.path.join(r, "distros/fedora-kde/scripts", name)
        dst = P("usr/local/lib/schema-init/scripts/" + name)
        if os.path.exists(src):
            _copy_into(src, dst, manifest, dry_run)
            written.append(dst)
            if not dry_run:
                manifest.add_file("/usr/local/lib/schema-init/scripts/" + name)
    for name in ps["config"]:
        src = os.path.join(r, "distros/fedora-kde/config", name)
        dst = P("etc/schema-init/config/" + name)
        if os.path.exists(src):
            _copy_into(src, dst, manifest, dry_run)
            written.append(dst)
            if not dry_run:
                manifest.add_file("/etc/schema-init/config/" + name)
    for name in ps["service"]:
        for base in ("distros/fedora-installer/rail/services",
                     "distros/fedora-kde/services"):
            src = os.path.join(r, base, name + ".svc")
            if os.path.exists(src):
                dst = P("etc/schema-init/services/" + name + ".svc")
                _copy_into(src, dst, manifest, dry_run)
                written.append(dst)
                if not dry_run:
                    manifest.add_file("/etc/schema-init/services/" + name + ".svc")
                break
    return written
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_deploy_profile.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_deploy_profile.py
git commit -m "schema-migrate: deploy the curated prevent-set (excluded units never copied)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 9: Generate host-specific units (hostname + mounts)

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_hostunits.py`

**Interfaces:**
- Consumes: `P`, `Manifest`.
- Produces: `def generate_host_units(profile, manifest, dry_run=False) -> list[str]` — for each mount in `profile["mounts"]`, write `P("etc/schema-init/services/mount-<slug>.svc")` (a `oneshot=1 needs_root=1` unit whose `exec=/bin/mount` with the src/target), slug = target with `/`→`-` (root = `root`). Write `P("etc/schema-init/services/hostname.svc")` only if not already provided by the prevent-set. Record each in the manifest. Returns destination paths.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""host unit generation tests."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

profile = {"mounts": [
    {"src": "UUID=bbb", "target": "/home", "fstype": "xfs", "opts": "defaults"},
    {"src": "UUID=ccc", "target": "/mnt/data", "fstype": "ext4", "opts": "defaults"}]}
m = sm.Manifest()
dst = sm.generate_host_units(profile, m)
home_unit = os.path.join(root, "etc/schema-init/services/mount-home.svc")
data_unit = os.path.join(root, "etc/schema-init/services/mount-mnt-data.svc")
check("home mount unit written", os.path.exists(home_unit))
check("nested mount slug is path-safe", os.path.exists(data_unit))
body = open(home_unit).read()
check("unit mounts the right target", "/home" in body and "UUID=bbb" in body)
check("unit is oneshot needs_root", "oneshot=1" in body and "needs_root=1" in body)
check("manifest recorded units", any("mount-home.svc" in f for f in m.files))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_hostunits.py`
Expected: FAIL — `generate_host_units` not defined.

- [ ] **Step 3: Implement**

```python
def _mount_slug(target):
    if target == "/":
        return "root"
    return target.strip("/").replace("/", "-")


def generate_host_units(profile, manifest, dry_run=False):
    written = []
    for mnt in profile.get("mounts", []):
        slug = _mount_slug(mnt["target"])
        rel = "etc/schema-init/services/mount-%s.svc" % slug
        body = ("name=mount-%s\n"
                "exec=/bin/mount\n"
                "args=-t %s -o %s %s %s\n"
                "oneshot=1\n"
                "needs_root=1\n"
                "critical=0\n") % (slug, mnt["fstype"], mnt.get("opts", "defaults"),
                                   mnt["src"], mnt["target"])
        written.append(P(rel))
        if not dry_run:
            os.makedirs(os.path.dirname(P(rel)), exist_ok=True)
            open(P(rel), "w").write(body)
            manifest.add_file("/" + rel)
    return written
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_hostunits.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_hostunits.py
git commit -m "schema-migrate: generate host mount units from the profile

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 10: make install + package install (injected runner)

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_packages.py`

**Interfaces:**
- Consumes: `repo`, `P`, `Manifest`.
- Produces:
  - `PREVENT_PACKAGES = ["libavcodec-freeworld", "egl-wayland", "seatd"]`.
  - `def _installed(pkg, run) -> bool` — `rpm -q <pkg>` via `run`, True on returncode 0.
  - `def install_packages(manifest, run=subprocess.run, dry_run=False) -> list[str]` — for each prevent package not already installed, `dnf install -y <pkg>` (via `run`) and record it in the manifest; return the list actually installed. Already-present packages are neither installed nor recorded (uninstall must never remove a pre-existing package).
  - `def run_make_install(run=subprocess.run, dry_run=False) -> None` — `make -C <repo> install DESTDIR=<ROOT> PREFIX=/usr` via `run`.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""package + make-install tests — inject the runner, no real dnf/make."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

calls = []
# pretend egl-wayland is already installed, the others are not
def fake_run(argv, **kw):
    calls.append(argv)
    rc = 0 if (argv[:2] == ["rpm", "-q"] and "egl-wayland" in argv) else (0 if argv[:2] != ["rpm", "-q"] else 1)
    return type("R", (), {"returncode": rc, "stdout": ""})()

m = sm.Manifest()
installed = sm.install_packages(m, run=fake_run)
check("installs the missing packages", "libavcodec-freeworld" in installed)
check("skips the already-present package", "egl-wayland" not in installed)
check("records only newly installed", "egl-wayland" not in m.packages and "libavcodec-freeworld" in m.packages)
check("dnf install actually called", any(c[:2] == ["dnf", "install"] for c in calls))

calls.clear()
sm.run_make_install(run=fake_run)
check("make install targets DESTDIR + PREFIX",
      any(c[0] == "make" and any("DESTDIR=" in x for x in c) and "PREFIX=/usr" in c for c in calls))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_packages.py`
Expected: FAIL — `install_packages` not defined.

- [ ] **Step 3: Implement**

```python
PREVENT_PACKAGES = ["libavcodec-freeworld", "egl-wayland", "seatd"]


def _installed(pkg, run):
    try:
        return run(["rpm", "-q", pkg], capture_output=True, text=True).returncode == 0
    except Exception:
        return False


def install_packages(manifest, run=subprocess.run, dry_run=False):
    done = []
    for pkg in PREVENT_PACKAGES:
        if _installed(pkg, run):
            continue
        done.append(pkg)
        if not dry_run:
            run(["dnf", "install", "-y", pkg], check=False)
            manifest.add_package(pkg)
    return done


def run_make_install(run=subprocess.run, dry_run=False):
    if dry_run:
        return
    run(["make", "-C", repo(), "install", "DESTDIR=" + ROOT.rstrip("/"), "PREFIX=/usr"],
        check=False)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_packages.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_packages.py
git commit -m "schema-migrate: make install + additive prevent-package install

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 11: Driver CLI — sequence the phases

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Test: `tests/test_migrate_cli.py`

**Interfaces:**
- Consumes: everything above.
- Produces:
  - `def do_deploy(run=subprocess.run, dry_run=False) -> Manifest` — build+write profile, then `run_make_install`, `deploy_prevent_set`, `generate_host_units`, `install_packages`, `add_boot_entry(profile["kernel"])` (record it in the manifest), `manifest.save()`. In `dry_run`, calls each helper with `dry_run=True` and does not save.
  - `def main(argv, run=subprocess.run) -> int` — argparse: `--discover`, `--deploy`, `--dry-run`, `--uninstall`; default (no flag) = discover-preview + deploy. `--discover` writes the profile and prints it, changes nothing else. Refuses (exit 2) if `detect_platform` returns None. Guards re-migration: if a manifest already exists and not `--uninstall`, print "already migrated" and exit 0.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""CLI sequencing tests — full deploy against a temp root with injected runner."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
fakerepo = tempfile.mkdtemp(); os.environ["MIGRATE_REPO"] = fakerepo
os.environ["MIGRATE_KERNEL"] = "6.10.0"
# minimal system + repo
for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "etc/os-release"), "w").write("ID=fedora\n")
open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
open(os.path.join(root, "etc/fstab"), "w").write("UUID=aaa / ext4 defaults 0 1\n")
open(os.path.join(root, "etc/passwd"), "w").write("jandoe:x:1000:1000::/home/jandoe:/bin/bash\n")
open(os.path.join(root, "boot/loader/entries/f.conf"), "w").write(
    "title Fedora\nversion 6.10.0\noptions root=UUID=aaa ro\n")
for d in ("distros/fedora-kde/scripts", "distros/fedora-installer/rail/services",
          "distros/fedora-installer/migrate"):
    os.makedirs(os.path.join(fakerepo, d))
open(os.path.join(fakerepo, "distros/fedora-kde/scripts/plasma-session-start.sh"), "w").write("#!/bin/sh\n")
open(os.path.join(fakerepo, "distros/fedora-installer/rail/services/dbus.svc"), "w").write("name=dbus\n")
open(os.path.join(fakerepo, "distros/fedora-installer/migrate/prevent-set.list"), "w").write(
    "script plasma-session-start.sh\nservice dbus\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
sm._PREVENT_LIST_OVERRIDE = os.path.join(fakerepo, "distros/fedora-installer/migrate/prevent-set.list")
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

def fake_run(argv, **kw):
    return type("R", (), {"returncode": 1 if argv[:2] == ["rpm", "-q"] else 0, "stdout": ""})()

# dry-run writes nothing
rc = sm.main(["--dry-run"], run=fake_run)
check("dry-run exits 0", rc == 0)
check("dry-run wrote no services", not os.path.exists(os.path.join(root, "etc/schema-init/services")))

# full deploy
rc = sm.main([], run=fake_run)
check("deploy exits 0", rc == 0)
check("prevent-set landed", os.path.exists(os.path.join(root, "etc/schema-init/services/dbus.svc")))
check("boot entry created", os.path.exists(os.path.join(root, "boot/loader/entries/schema-init.conf")))
check("manifest saved", os.path.exists(os.path.join(root, sm.Manifest.PATH)))

# re-run guard
rc = sm.main([], run=fake_run)
check("re-migration is a no-op exit 0", rc == 0)

# uninstall
rc = sm.main(["--uninstall"], run=fake_run)
check("uninstall exits 0 + removes boot entry",
      rc == 0 and not os.path.exists(os.path.join(root, "boot/loader/entries/schema-init.conf")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_cli.py`
Expected: FAIL — `main` not defined.

- [ ] **Step 3: Implement**

```python
def do_deploy(run=subprocess.run, dry_run=False):
    profile = build_profile(run=run)
    if not dry_run:
        write_profile(profile)
    m = Manifest()
    run_make_install(run=run, dry_run=dry_run)
    deploy_prevent_set(m, dry_run=dry_run)
    generate_host_units(profile, m, dry_run=dry_run)
    install_packages(m, run=run, dry_run=dry_run)
    entry = add_boot_entry(profile["kernel"]) if not dry_run else None
    if not dry_run:
        m.set_boot_entry("/" + os.path.relpath(entry, ROOT))
        m.save()
    return m


def main(argv, run=subprocess.run):
    import argparse
    ap = argparse.ArgumentParser(prog="schema-migrate")
    ap.add_argument("--discover", action="store_true", help="preview the profile, change nothing")
    ap.add_argument("--deploy", action="store_true", help="run the flip")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, change nothing")
    ap.add_argument("--uninstall", action="store_true", help="reverse a prior migration")
    args = ap.parse_args(argv)

    if args.uninstall:
        res = uninstall(run=run)
        print("uninstalled: %d files, packages=%s" % (res["files_removed"], res["packages"]))
        return 0

    plat, why = detect_platform()
    if plat is None:
        print("refusing: " + why)
        return 2

    if args.discover:
        profile = build_profile(run=run)
        path = write_profile(profile)
        print(json.dumps(profile, indent=2))
        print("profile written to " + path)
        return 0

    if os.path.exists(P(Manifest.PATH)) and not args.dry_run:
        print("already migrated (manifest present) — run --uninstall to reverse")
        return 0

    do_deploy(run=run, dry_run=args.dry_run)
    print("dry-run complete — nothing changed" if args.dry_run
          else "deploy complete — reboot and pick the '(schema-init)' entry")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_cli.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py tests/test_migrate_cli.py
git commit -m "schema-migrate: driver CLI (discover/deploy/dry-run/uninstall + re-run guard)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

### Task 12: Finish phase — post-reboot report + rail wiring

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-migrate.py`
- Create: `distros/fedora-installer/rail/services/schema-migrate-finish.svc`
- Test: `tests/test_migrate_finish.py`

**Interfaces:**
- Consumes: `Manifest`, `P`, the profile JSON.
- Produces:
  - `def finish_report() -> str` — reads the profile JSON (`services.leftover`) and the doctor status file if present (`run/schema-init/doctor-status`), returns a text report naming what deployed, the doctor result, and the **un-ported leftover services** with the note that the translate step can carry them. Writes a once marker `run/schema-init/migrate-finished` so the oneshot self-disables.
  - `main` gains `--finish` → prints `finish_report()` and returns 0; the marker makes a second `--finish` a no-op line.

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""finish report tests."""
import os, sys, json, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
for d in ("var/lib/schema-init", "run/schema-init"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "var/lib/schema-init/migrate-profile.json"), "w").write(
    json.dumps({"platform": "fedora-kde", "services": {"leftover": ["tailscaled", "docker"]}}))
open(os.path.join(root, "run/schema-init/doctor-status"), "w").write("schema-doctor: GREEN\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

rep = sm.finish_report()
check("names the leftover services", "tailscaled" in rep and "docker" in rep)
check("shows the doctor result", "GREEN" in rep)
check("mentions the translate step", "translate" in rep.lower())
check("writes the once marker", os.path.exists(os.path.join(root, "run/schema-init/migrate-finished")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_migrate_finish.py`
Expected: FAIL — `finish_report` not defined.

- [ ] **Step 3: Implement + write the svc**

Add to the module:

```python
def finish_report():
    try:
        prof = json.load(open(P("var/lib/schema-init/migrate-profile.json")))
    except (OSError, ValueError):
        prof = {}
    leftover = prof.get("services", {}).get("leftover", [])
    try:
        doctor = open(P("run/schema-init/doctor-status")).read().strip()
    except OSError:
        doctor = "(schema-doctor status not found)"
    lines = ["=== schema-migrate: first-boot report ===", "",
             "platform: " + str(prof.get("platform", "?")), "",
             "schema-doctor: " + doctor, ""]
    if leftover:
        lines.append("Services still running under the old system that were NOT ported:")
        lines += ["  - " + s for s in leftover]
        lines.append("")
        lines.append("Run `schema-migrate --translate` to carry these over "
                     "(simple units auto-convert; complex ones are named, not dropped).")
    else:
        lines.append("No un-ported services — nothing left to translate.")
    marker = P("run/schema-init/migrate-finished")
    os.makedirs(os.path.dirname(marker), exist_ok=True)
    open(marker, "w").write("done\n")
    return "\n".join(lines)
```

Add `--finish` to `main`'s argparse and, near the top of `main` after parsing:

```python
    if args.finish:
        if os.path.exists(P("run/schema-init/migrate-finished")):
            print("migrate-finish already ran")
            return 0
        print(finish_report())
        return 0
```

(with `ap.add_argument("--finish", action="store_true", help="post-reboot report + translate offer")`)

Create `distros/fedora-installer/rail/services/schema-migrate-finish.svc`:

```
name=schema-migrate-finish
exec=/usr/local/bin/schema-migrate
args=--finish
dep=schema-doctor
oneshot=1
needs_root=1
critical=0
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_migrate_finish.py`
Expected: PASS.

- [ ] **Step 5: Run the whole migrate + doctor suite**

Run: `for t in tests/test_migrate_*.py tests/test_doctor_*.py; do python3 "$t" >/dev/null 2>&1 && echo "OK $t" || echo "FAIL $t"; done`
Expected: every line `OK`.

- [ ] **Step 6: Commit**

```bash
git add distros/fedora-installer/migrate/schema-migrate.py \
        distros/fedora-installer/rail/services/schema-migrate-finish.svc \
        tests/test_migrate_finish.py
git commit -m "schema-migrate: post-reboot finish report + rail wiring

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RyefFnMBET8HUB1zyFU8ke"
```

---

## Post-plan verification (before calling v1 done)

- All `tests/test_migrate_*.py` and `tests/test_doctor_*.py` green.
- **VM round-trip** (the real acceptance gate, not unit tests): install stock Fedora KDE in a VM, copy the repo/USB in, `sudo MIGRATE_REPO=<repo> python3 schema-migrate.py`, reboot, pick the `(schema-init)` entry, confirm the desktop comes up, `schema-doctor --check` is clean, `--finish` report is sane, then `--uninstall` + reboot returns to stock. Use `schema-vmtest` for the boot-as-PID1 half.
- Install path: ensure `schema-migrate.py` is installed to `/usr/local/bin/schema-migrate` (add to `make install` or the rail payload in a follow-up once the VM proves the flow).

## Deferred (designed-for, not in this plan)

- **`--translate`**: the systemd-unit → `.svc` translator offered by the finish report. Auto-converts simple `Type=simple`/`oneshot` units; names what it punts.
- **Multi-distro / multi-DE**: one `prevent-set.list` per profile; generalize `detect_platform` and the copy sources. *(Remembered as the next milestone after Fedora KDE is proven.)*
