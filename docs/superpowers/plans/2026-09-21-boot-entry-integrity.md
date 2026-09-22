# boot-entry-integrity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new SAFE-grade schema-doctor check that detects and auto-heals schema BLS boot entries that lose `init=/sbin/schema-init` or their host-specific `kernel-cmdline.d` args, and a `saved_entry` that gets repointed at a stock or dangling entry — plus make the kernel-install hook's boot-default pin actually stick, and give Jonathan a one-click desktop shortcut to check a box right after an update, before rebooting.

**Architecture:** One new `Check` subclass (`BootEntryIntegrity`) registered in schema-doctor's existing engine — rides the existing boot-time and 10-minute-periodic service entry points for free. A small, backward-compatible extension to `/etc/schema-init/boot-default`'s semantics (content becomes an optional pin) read by both the kernel-install hook and the new check. A `yad`-based desktop shortcut following the existing `firstboot-flip-wizard.sh` pattern, delegating the one privileged call through a narrowly-scoped sudoers entry.

**Tech Stack:** Python 3 stdlib (schema-doctor.py), POSIX sh (kernel-install hook), bash + `yad` (desktop shortcut), plain-script test runners (no pytest — this repo's existing `tests/test_doctor_*.py` and `tests/test_kernel_install_hook.py` are self-contained scripts with a `check()`/`results` pattern, `exit 0`/`1`).

**Spec:** `docs/superpowers/specs/2026-09-21-boot-entry-integrity-design.md`

## Global Constraints

- `schema-init` svc `exec=` takes exactly one path, never `path arg arg` — args always go in separate `args=` lines (bit the original doctor deploy; not touched by this plan, but relevant if any `.svc` file is edited).
- Deploy to a live schema-init box via `kill -HUP 1`, **never** `restart`.
- No wildcards in sudoers NOPASSWD entries — exact command + args only (existing pattern: `distros/fedora-installer/migrate/schema-wizard.sudoers`).
- `schema-doctor` checks must never crash the run: every new method follows the existing `Check` contract (`detect`/`heal`/`snapshot`/`back_out`/`verify`) exactly, no new exceptions escaping `heal()`/`detect()` beyond what `_safe_detect_all()` already catches.
- Boot-critical code (the kernel-install hook, the doctor's `heal()`) must boot-test under `schema-vmtest` before touching Optiplex live again — this is non-negotiable given tonight's incident.

---

## Task 1: `BootEntryIntegrity.detect()` — entries missing `init=` or `kernel-cmdline.d` tokens

**Files:**
- Modify: `scripts/schema-doctor.py:9-19` (imports), `scripts/schema-doctor.py:816` (insert after `REGISTRY.append(NvidiaWaylandEgl())`, before `def read_config():`)
- Create: `tests/test_doctor_boot_entry_integrity.py`

**Interfaces:**
- Consumes: `Check`, `Finding`, `SAFE`, `ROOT` (module-level, already defined in `scripts/schema-doctor.py`)
- Produces: `BootEntryIntegrity` class with `name = "boot-entry-integrity"`; helper functions `_cmdline_extra_tokens() -> list[str]` and `_options_line(path) -> (int|None, str|None)` (line index + text of the entry's `options` line) that later tasks (heal, saved_entry logic) also use.

- [ ] **Step 1: Write the failing test**

Create `tests/test_doctor_boot_entry_integrity.py`:

```python
#!/usr/bin/env python3
"""boot-entry-integrity tests — schema BLS entries keep init=schema-init and
host kernel-cmdline.d args; saved_entry stays on a real schema entry.

  ./tests/test_doctor_boot_entry_integrity.py    exit 0 all pass, 1 any fail
"""
import os
import sys
import tempfile
import importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def new_root(entries=True, extras=True):
    root = tempfile.mkdtemp(prefix='schema-doctor-bei-')
    ent_dir = os.path.join(root, 'boot/loader/entries')
    os.makedirs(ent_dir)
    conf_root = os.path.join(root, 'etc/schema-init')
    os.makedirs(conf_root)
    if extras:
        os.makedirs(os.path.join(conf_root, 'kernel-cmdline.d'))
        with open(os.path.join(conf_root, 'kernel-cmdline.d/10-radeon.conf'), 'w') as f:
            f.write('# optiplex GPU\nmodprobe.blacklist=radeon\n')
    bind = os.path.join(root, 'bin')
    os.makedirs(bind)
    grubenv_state = os.path.join(root, 'grubenv-state.txt')
    stub = os.path.join(bind, 'grub2-editenv-stub.sh')
    with open(stub, 'w') as f:
        f.write(f'''#!/bin/sh
case "$2" in
  list) [ -f "{grubenv_state}" ] && cat "{grubenv_state}" ;;
  set)
    shift 2
    for a in "$@"; do
      case "$a" in saved_entry=*) echo "$a" > "{grubenv_state}" ;; esac
    done
    ;;
esac
exit 0
''')
    os.chmod(stub, 0o755)
    return root, ent_dir, conf_root, bind, stub, grubenv_state


def write_entry(ent_dir, name, options):
    with open(os.path.join(ent_dir, f'{name}.conf'), 'w') as f:
        f.write(f'title Fedora Linux\nversion x\noptions {options}\n')


def load_module(root, stub):
    os.environ['DOCTOR_ROOT'] = root
    os.environ['SCHEMA_DOCTOR_GRUB2_EDITENV'] = stub
    spec = importlib.util.spec_from_file_location(
        'schema_doctor', os.path.join(REPO, 'scripts', 'schema-doctor.py'))
    sd = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sd)
    return sd


def test_clean_entry_detects_none():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root init=/sbin/schema-init '
                'modprobe.blacklist=radeon')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('clean entry: detect returns None', f is None, f.detail if f else '')


def test_missing_init_detected():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root modprobe.blacklist=radeon')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('missing init=: detected', f is not None)
    check('missing init=: names the entry', 'schema-7.1.12' in (f.detail if f else ''), f.detail if f else '')


def test_missing_extra_only_detected():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root init=/sbin/schema-init')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('missing extra only (init= intact): still detected', f is not None)


def test_tonights_actual_shape_all_entries_broken():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    for name in ('schema-ssd-7.1.12-200.fc44.x86_64', 'schema-7.1.12-200.fc44.x86_64',
                 'schema-7.1.10-200.fc44.x86_64', 'schema-7.1.8-200.fc44.x86_64'):
        write_entry(ent_dir, name, 'root=/dev/sda2 ro rootflags=subvol=root rhgb quiet')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('all 4 entries stripped: detected', f is not None)
    for name in ('schema-ssd-7.1.12', 'schema-7.1.12', 'schema-7.1.10', 'schema-7.1.8'):
        check(f'all-stripped: {name} named in detail', name in (f.detail if f else ''), f.detail if f else '')


def test_no_schema_entries_clean():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    c = sd.BootEntryIntegrity()
    check('no schema-*.conf at all (greybox/eli model): clean', c.detect() is None)


def main():
    print('boot-entry-integrity tests\n')
    for fn in (test_clean_entry_detects_none, test_missing_init_detected,
               test_missing_extra_only_detected, test_tonights_actual_shape_all_entries_broken,
               test_no_schema_entries_clean):
        print(fn.__name__)
        fn()
        print()
    passed = sum(1 for r in results if r)
    print(f"{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: `AttributeError: module 'schema_doctor' has no attribute 'BootEntryIntegrity'` (or similar import failure) — `FAIL` overall, exit 1.

- [ ] **Step 3: Add `shutil` import**

In `scripts/schema-doctor.py`, the imports block currently reads (lines 9-19):

```python
import fcntl
import glob
import json
import os
import re
import stat
import struct
import subprocess
import sys
import time
```

Add `shutil` alphabetically:

```python
import fcntl
import glob
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import time
```

- [ ] **Step 4: Implement `BootEntryIntegrity.detect()` (entries half only)**

Insert immediately after line 816 (`REGISTRY.append(NvidiaWaylandEgl())`) and its blank lines, before `def read_config():`:

```python
INIT_RE = re.compile(r"init=\S*schema-init")


def _cmdline_extra_tokens():
    """Host-specific kernel cmdline tokens from kernel-cmdline.d/*.conf, parsed
    the same way the kernel-install hook's extra_args() does."""
    d = os.path.join(ROOT, "etc/schema-init/kernel-cmdline.d")
    try:
        names = sorted(os.listdir(d))
    except OSError:
        return []
    tokens = []
    for n in names:
        if not n.endswith(".conf"):
            continue
        for line in open(os.path.join(d, n)):
            line = line.split("#", 1)[0].strip()
            if line:
                tokens.extend(line.split())
    return tokens


def _resolve_schema_init_bin():
    override = os.environ.get("SCHEMA_INIT_BIN")
    if override:
        return override
    return shutil.which("schema-init") or "/usr/bin/schema-init"


class BootEntryIntegrity(Check):
    name = "boot-entry-integrity"
    summary = "schema BLS entries keep init=schema-init; saved_entry stays on one"
    grade = SAFE

    def _entries(self):
        return sorted(glob.glob(os.path.join(ROOT, "boot/loader/entries/schema-*.conf")))

    def _options_line(self, path):
        lines = open(path).readlines()
        for i, line in enumerate(lines):
            if line.startswith("options "):
                return i, line.rstrip("\n")
        return None, None

    def _missing_tokens(self, options_line, extras):
        missing = []
        if not INIT_RE.search(options_line or ""):
            missing.append("init=schema-init")
        for tok in extras:
            if tok not in (options_line or ""):
                missing.append(tok)
        return missing

    def detect(self):
        entries = self._entries()
        if not entries:
            return None
        extras = _cmdline_extra_tokens()
        broken = {}
        for path in entries:
            _, line = self._options_line(path)
            missing = self._missing_tokens(line, extras)
            if missing:
                broken[os.path.basename(path)] = missing
        if not broken:
            return None
        detail = "entries missing tokens: " + "; ".join(
            f"{name}: {','.join(toks)}" for name, toks in broken.items())
        return Finding(
            detail=detail,
            oracle_said="every schema-*.conf carries init=<schema-init> plus this "
                        "host's kernel-cmdline.d extras",
            healable=True)


REGISTRY.append(BootEntryIntegrity())
```

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: all 5 tests `PASS`, `5/5 passed`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_boot_entry_integrity.py
git commit -m "feat(doctor): boot-entry-integrity detects missing init= and cmdline.d extras"
```

---

## Task 2: `BootEntryIntegrity.detect()` — `saved_entry` pointed at a non-schema or dangling entry

**Files:**
- Modify: `scripts/schema-doctor.py` (the `detect()` method and helpers added in Task 1)
- Modify: `tests/test_doctor_boot_entry_integrity.py`

**Interfaces:**
- Consumes: `_entries()`, `_missing_tokens()`, `_options_line()` from Task 1
- Produces: `_read_saved_entry() -> str|None` and `_set_saved_entry(name)` (module-level helpers, env-override via `SCHEMA_DOCTOR_GRUB2_EDITENV`) — Task 4/5 (heal) also use these.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_doctor_boot_entry_integrity.py`, before `def main():`:

```python
def set_saved_entry(grubenv_state, name):
    with open(grubenv_state, 'w') as f:
        f.write(f'saved_entry={name}\n')


def test_saved_entry_on_stock_detected():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    set_saved_entry(grubenv_state, '8ac661a02a5647aaa4f14e6f78f77879-7.2.6-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('saved_entry on stock kernel: detected', f is not None)
    check('saved_entry on stock kernel: named in detail', 'saved_entry' in (f.detail if f else ''), f.detail if f else '')


def test_saved_entry_dangling_detected():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    # saved_entry NAME looks like ours but the .conf file is gone (kernel removed)
    set_saved_entry(grubenv_state, 'schema-6.9.9-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('dangling saved_entry (schema- prefix, no file): detected', f is not None)


def test_saved_entry_on_valid_schema_clean():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    set_saved_entry(grubenv_state, 'schema-7.1.12-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    check('saved_entry on a real, valid schema entry: clean', c.detect() is None)
```

Add these four function names to the loop in `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: `test_saved_entry_on_stock_detected` and `test_saved_entry_dangling_detected` FAIL (detect still returns `None` — saved_entry isn't checked yet); `test_saved_entry_on_valid_schema_clean` passes trivially.

- [ ] **Step 3: Implement `_read_saved_entry()`/`_set_saved_entry()` and extend `detect()`**

Add helpers near `_resolve_schema_init_bin()` (Task 1's insertion point):

```python
def _read_saved_entry():
    cmd = os.environ.get("SCHEMA_DOCTOR_GRUB2_EDITENV", "grub2-editenv")
    try:
        r = subprocess.run([cmd, "-", "list"], capture_output=True, text=True)
    except OSError:
        return None
    for line in r.stdout.splitlines():
        if line.startswith("saved_entry="):
            return line[len("saved_entry="):].strip()
    return None


def _set_saved_entry(name):
    cmd = os.environ.get("SCHEMA_DOCTOR_GRUB2_EDITENV", "grub2-editenv")
    subprocess.run([cmd, "-", "set", f"saved_entry={name}"], check=False)
```

Replace `BootEntryIntegrity.detect()`'s body with:

```python
    def _saved_entry_problem(self):
        saved = _read_saved_entry()
        if saved is None:
            return None
        if not saved.startswith("schema-"):
            return f"saved_entry={saved} is not a schema entry"
        if not os.path.isfile(os.path.join(ROOT, "boot/loader/entries", saved + ".conf")):
            return f"saved_entry={saved} has no entry file (dangling)"
        return None

    def detect(self):
        entries = self._entries()
        extras = _cmdline_extra_tokens()
        broken = {}
        for path in entries:
            _, line = self._options_line(path)
            missing = self._missing_tokens(line, extras)
            if missing:
                broken[os.path.basename(path)] = missing
        saved_problem = self._saved_entry_problem() if entries else None
        if not broken and not saved_problem:
            return None
        parts = []
        if broken:
            parts.append("entries missing tokens: " + "; ".join(
                f"{name}: {','.join(toks)}" for name, toks in broken.items()))
        if saved_problem:
            parts.append(saved_problem)
        return Finding(
            detail="; ".join(parts),
            oracle_said="every schema-*.conf carries init=<schema-init> plus this "
                        "host's kernel-cmdline.d extras, and saved_entry points at "
                        "one of them",
            healable=True)
```

Note: `saved_problem` is only checked `if entries` — on greybox/eli (no `schema-*.conf` at all) this check stays a no-op even if `saved_entry` happens to look unusual, matching the spec's scoping (this check only applies to the separate-entry model).

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: all 8 tests `PASS`, `8/8 passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_boot_entry_integrity.py
git commit -m "feat(doctor): boot-entry-integrity detects saved_entry on stock/dangling entry"
```

---

## Task 3: `BootEntryIntegrity.heal()` — fix entries (strip stale `init=`, restore `init=` + extras)

**Files:**
- Modify: `scripts/schema-doctor.py`
- Modify: `tests/test_doctor_boot_entry_integrity.py`

**Interfaces:**
- Consumes: `_entries()`, `_options_line()`, `_missing_tokens()`, `_cmdline_extra_tokens()`, `_resolve_schema_init_bin()`
- Produces: `_rewrite_options(path, idx, new_line)` (atomic options-line rewrite) — Task 5 (`back_out`) also uses this.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_doctor_boot_entry_integrity.py`:

```python
def read_options(ent_dir, name):
    with open(os.path.join(ent_dir, f'{name}.conf')) as f:
        for line in f:
            if line.startswith('options '):
                return line.rstrip('\n')
    return ''


def test_heal_restores_missing_init_and_extras():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root rhgb quiet')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    c.heal(f)
    opts = read_options(ent_dir, 'schema-7.1.12-200.fc44.x86_64')
    check('heal: init= restored', 'init=/sbin/schema-init' in opts, opts)
    check('heal: extra restored', 'modprobe.blacklist=radeon' in opts, opts)
    check('heal: verify() clean', c.verify() is True)


def test_heal_strips_duplicate_stale_init():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/usr/lib/systemd/systemd modprobe.blacklist=radeon')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    c.heal(f)
    opts = read_options(ent_dir, 'schema-7.1.12-200.fc44.x86_64')
    check('heal: exactly one init= token', opts.count('init=') == 1, opts)
    check('heal: the surviving init= is schema-init', 'init=/sbin/schema-init' in opts, opts)


def test_heal_idempotent():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root rhgb quiet')
    c = sd.BootEntryIntegrity()
    c.heal(c.detect())
    c.heal(c.detect() or sd.Finding('x'))
    opts = read_options(ent_dir, 'schema-7.1.12-200.fc44.x86_64')
    check('heal idempotent: still exactly one init=', opts.count('init=') == 1, opts)
    check('heal idempotent: verify() clean', c.verify() is True)
```

Add the three function names to `main()`'s loop.

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: all three new tests FAIL (`heal` is still `Check.heal`, a no-op `pass`).

- [ ] **Step 3: Implement `heal()` (entries half) and `_rewrite_options()`**

Add to `BootEntryIntegrity`:

```python
    def _rewrite_options(self, path, idx, new_line):
        lines = open(path).readlines()
        lines[idx] = new_line + "\n"
        tmp = path + ".tmp"
        with open(tmp, "w") as fh:
            fh.writelines(lines)
        os.replace(tmp, path)

    def _heal_entries(self, extras):
        for path in self._entries():
            idx, line = self._options_line(path)
            if line is None:
                continue
            missing = self._missing_tokens(line, extras)
            if not missing:
                continue
            new_line = re.sub(r"init=\S*\s*", "", line).rstrip()
            new_line += f" init={_resolve_schema_init_bin()}"
            for tok in extras:
                if tok not in new_line:
                    new_line += f" {tok}"
            self._rewrite_options(path, idx, new_line)

    def heal(self, f):
        extras = _cmdline_extra_tokens()
        self._heal_entries(extras)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: all 11 tests `PASS`, `11/11 passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_boot_entry_integrity.py
git commit -m "feat(doctor): boot-entry-integrity heals missing init=/extras, strips stale init="
```

---

## Task 4: `BootEntryIntegrity.heal()` — fix `saved_entry` (pin-aware, normalized)

**Files:**
- Modify: `scripts/schema-doctor.py`
- Modify: `tests/test_doctor_boot_entry_integrity.py`

**Interfaces:**
- Consumes: `_read_saved_entry()`, `_set_saved_entry()`, `_entries()`
- Produces: `_normalize_pin(raw) -> str`, `BootEntryIntegrity._pin_target()`, `BootEntryIntegrity._newest_entry()`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_doctor_boot_entry_integrity.py`:

```python
def test_heal_saved_entry_no_pin_picks_newest():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.10-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    set_saved_entry(grubenv_state, '8ac661a02a5647aaa4f14e6f78f77879-7.2.6-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    c.heal(c.detect())
    check('no pin: saved_entry healed to newest schema entry',
          open(grubenv_state).read().strip() == 'saved_entry=schema-7.1.12-200.fc44.x86_64',
          open(grubenv_state).read())


def test_heal_saved_entry_pin_wins_over_newest():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-ssd-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    write_entry(ent_dir, 'schema-7.2.6-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    with open(os.path.join(conf_root, 'boot-default'), 'w') as fh:
        fh.write('schema-ssd-7.1.12-200.fc44.x86_64')
    set_saved_entry(grubenv_state, '8ac661a02a5647aaa4f14e6f78f77879-7.2.6-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    c.heal(c.detect())
    check('pin: saved_entry healed to the pin, not the newer schema-7.2.6',
          open(grubenv_state).read().strip() == 'saved_entry=schema-ssd-7.1.12-200.fc44.x86_64',
          open(grubenv_state).read())


def test_heal_saved_entry_broken_pin_falls_back():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    with open(os.path.join(conf_root, 'boot-default'), 'w') as fh:
        fh.write('schema-9.9.9-nonexistent.fc44.x86_64')
    set_saved_entry(grubenv_state, '8ac661a02a5647aaa4f14e6f78f77879-7.2.6-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    c.heal(c.detect())
    check('broken pin: falls back to newest resolvable schema entry',
          open(grubenv_state).read().strip() == 'saved_entry=schema-7.1.12-200.fc44.x86_64',
          open(grubenv_state).read())


def test_heal_saved_entry_pin_normalized():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-ssd-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    with open(os.path.join(conf_root, 'boot-default'), 'w') as fh:
        fh.write('  schema-ssd-7.1.12-200.fc44.x86_64.conf  \n')
    set_saved_entry(grubenv_state, '8ac661a02a5647aaa4f14e6f78f77879-7.1.12-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    c.heal(c.detect())
    check('normalized pin (whitespace + .conf suffix): still resolves',
          open(grubenv_state).read().strip() == 'saved_entry=schema-ssd-7.1.12-200.fc44.x86_64',
          open(grubenv_state).read())
```

Add the four function names to `main()`'s loop.

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: all four new tests FAIL (`saved_entry` never gets written — `heal()` doesn't touch it yet).

- [ ] **Step 3: Implement pin resolution and extend `heal()`**

Add near the other module-level helpers:

```python
def _normalize_pin(raw):
    pin = raw.strip()
    if pin.endswith(".conf"):
        pin = pin[: -len(".conf")]
    return pin
```

Add to `BootEntryIntegrity`:

```python
    def _pin_target(self):
        marker = os.path.join(ROOT, "etc/schema-init/boot-default")
        try:
            raw = open(marker).read()
        except OSError:
            return None
        pin = _normalize_pin(raw)
        if pin and os.path.isfile(os.path.join(ROOT, "boot/loader/entries", pin + ".conf")):
            return pin
        return None

    def _newest_entry(self):
        entries = self._entries()
        if not entries:
            return None
        names = [os.path.basename(e)[len("schema-"):-len(".conf")] for e in entries]
        r = subprocess.run(["sort", "-V"], input="\n".join(names),
                            capture_output=True, text=True)
        ordered = [n for n in r.stdout.splitlines() if n]
        return f"schema-{ordered[-1]}" if ordered else None

    def _heal_saved_entry(self):
        problem = self._saved_entry_problem()
        if problem is None:
            return
        target = self._pin_target() or self._newest_entry()
        if target:
            _set_saved_entry(target)
```

Replace `heal()`:

```python
    def heal(self, f):
        extras = _cmdline_extra_tokens()
        self._heal_entries(extras)
        self._heal_saved_entry()
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: all 15 tests `PASS`, `15/15 passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_boot_entry_integrity.py
git commit -m "feat(doctor): boot-entry-integrity heals saved_entry, pin-aware and normalized"
```

---

## Task 5: `snapshot()` / `back_out()` and full registration check

**Files:**
- Modify: `scripts/schema-doctor.py`
- Modify: `tests/test_doctor_boot_entry_integrity.py`

**Interfaces:**
- Consumes: `_entries()`, `_options_line()`, `_rewrite_options()`, `_read_saved_entry()`, `_set_saved_entry()`
- Produces: `BootEntryIntegrity.snapshot()`, `BootEntryIntegrity.back_out()` — completes the `Check` contract.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_doctor_boot_entry_integrity.py`:

```python
def test_back_out_restores_original_state():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root rhgb quiet')
    set_saved_entry(grubenv_state, '8ac661a02a5647aaa4f14e6f78f77879-7.2.6-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    snap = c.snapshot()
    c.heal(f)
    check('back_out setup: heal changed the options line',
          'init=' in read_options(ent_dir, 'schema-7.1.12-200.fc44.x86_64'))
    c.back_out(snap)
    opts = read_options(ent_dir, 'schema-7.1.12-200.fc44.x86_64')
    check('back_out: options line restored exactly',
          opts == 'options root=/dev/sda2 ro rootflags=subvol=root rhgb quiet', opts)
    check('back_out: saved_entry restored',
          open(grubenv_state).read().strip() ==
          'saved_entry=8ac661a02a5647aaa4f14e6f78f77879-7.2.6-200.fc44.x86_64',
          open(grubenv_state).read())


def test_registered_in_registry():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    names = [c.name for c in sd.REGISTRY]
    check('boot-entry-integrity is in REGISTRY', 'boot-entry-integrity' in names, str(names))
```

Add both function names to `main()`'s loop.

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: `test_back_out_restores_original_state` FAILs (`back_out` is still `Check.back_out`, a no-op); `test_registered_in_registry` already passes (Task 1 registered it).

- [ ] **Step 3: Implement `snapshot()` and `back_out()`**

Add to `BootEntryIntegrity`:

```python
    def snapshot(self):
        snap = {"entries": {}, "saved_entry": _read_saved_entry()}
        for path in self._entries():
            _, line = self._options_line(path)
            snap["entries"][path] = line
        return snap

    def back_out(self, snap):
        for path, line in snap.get("entries", {}).items():
            if line is None:
                continue
            idx, _ = self._options_line(path)
            if idx is not None:
                self._rewrite_options(path, idx, line)
        if snap.get("saved_entry") is not None:
            _set_saved_entry(snap["saved_entry"])
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_doctor_boot_entry_integrity.py`
Expected: all 17 tests `PASS`, `17/17 passed`, exit 0.

- [ ] **Step 5: Run the full doctor test suite to check for regressions**

Run: `for t in tests/test_doctor_*.py; do echo "== $t =="; python3 "$t" || exit 1; done`
Expected: every existing doctor test file still exits 0 (the new check must not change any existing check's behavior — it's purely additive).

- [ ] **Step 6: Commit**

```bash
git add scripts/schema-doctor.py tests/test_doctor_boot_entry_integrity.py
git commit -m "feat(doctor): boot-entry-integrity snapshot/back_out completes the Check contract"
```

---

## Task 6: kernel-install hook — actively enforce the pin, normalize marker content

**Files:**
- Modify: `distros/shared/kernel-install/99-schema-init.install`
- Modify: `tests/test_kernel_install_hook.py`

**Interfaces:**
- Consumes: existing `DEFAULT_MARKER`, `ENTRIES` shell variables already defined in the hook
- Produces: updated `set_default_to()` — same call signature (`set_default_to "$VERSION"`), same two call sites (`add` and `remove` cases), unchanged.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_kernel_install_hook.py`, before `def main():`:

```python
def test_pin_enforced_over_stock():
    root, boot, entries, conf_root, rec, bind = new_tree()
    pinned = 'schema-7.1.10-200.fc44.x86_64'
    with open(os.path.join(entries, f'{pinned}.conf'), 'w') as f:
        f.write('title pinned\nversion 7.1.10-200.fc44.x86_64\n'
                 'options ro init=/usr/bin/schema-init\n')
    with open(os.path.join(conf_root, 'boot-default'), 'w') as f:
        f.write(pinned)
    run('add', VER, boot, conf_root, bind)
    log = open(rec).read() if os.path.isfile(rec) else ''
    check('pin: saved_entry actively set to the pin, not the new kernel',
          f'saved_entry={pinned}' in log, log)
    check('pin: new kernel version NOT left as the default',
          f'saved_entry=schema-{VER}' not in log, log)


def test_pin_broken_falls_back_to_advance():
    root, boot, entries, conf_root, rec, bind = new_tree()
    with open(os.path.join(conf_root, 'boot-default'), 'w') as f:
        f.write('schema-9.9.9-nonexistent.fc44.x86_64')
    run('add', VER, boot, conf_root, bind)
    log = open(rec).read() if os.path.isfile(rec) else ''
    check('pin-broken: falls back to advancing to the new kernel',
          f'saved_entry=schema-{VER}' in log, log)


def test_marker_whitespace_and_conf_suffix_normalized():
    root, boot, entries, conf_root, rec, bind = new_tree()
    pinned = 'schema-7.1.10-200.fc44.x86_64'
    with open(os.path.join(entries, f'{pinned}.conf'), 'w') as f:
        f.write('title pinned\nversion 7.1.10-200.fc44.x86_64\n'
                 'options ro init=/usr/bin/schema-init\n')
    with open(os.path.join(conf_root, 'boot-default'), 'w') as f:
        f.write(f'  {pinned}.conf  \n')
    run('add', VER, boot, conf_root, bind)
    log = open(rec).read() if os.path.isfile(rec) else ''
    check('marker normalization: whitespace + .conf suffix still resolves the pin',
          f'saved_entry={pinned}' in log, log)
```

Add the three function names to `main()`'s loop (alongside the existing 7).

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_kernel_install_hook.py`
Expected: `test_pin_enforced_over_stock` and `test_marker_whitespace_and_conf_suffix_normalized` FAIL — the current `set_default_to()` no-ops (`return 0`) when a pin is resolvable, so `grubenv-set.log` never gets written at all, so `f'saved_entry={pinned}' in log` is `False`. `test_pin_broken_falls_back_to_advance` already passes (today's fallback-to-newest behavior already does this by accident, since an unresolvable pin makes today's code fall through — confirm it still passes after the rewrite in Step 3).

- [ ] **Step 3: Rewrite `set_default_to()`**

In `distros/shared/kernel-install/99-schema-init.install`, replace:

```sh
set_default_to() {
    [ -f "$DEFAULT_MARKER" ] || return 0
    command -v grub2-editenv >/dev/null 2>&1 || return 0
    grub2-editenv - set "saved_entry=schema-$1" 2>/dev/null || true
}
```

with:

```sh
set_default_to() {
    # $1 = kernel version being added/removed (the "natural" candidate)
    [ -f "$DEFAULT_MARKER" ] || return 0
    pin="$(cat "$DEFAULT_MARKER" 2>/dev/null | tr -d '\r\n ')"
    pin="${pin%.conf}"
    command -v grub2-editenv >/dev/null 2>&1 || return 0
    if [ -n "$pin" ] && [ -f "$ENTRIES/$pin.conf" ]; then
        grub2-editenv - set "saved_entry=$pin" 2>/dev/null || true
        return 0
    fi
    grub2-editenv - set "saved_entry=schema-$1" 2>/dev/null || true
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_kernel_install_hook.py`
Expected: `10/10 passed` (7 original + 3 new), exit 0.

- [ ] **Step 5: Commit**

```bash
git add distros/shared/kernel-install/99-schema-init.install tests/test_kernel_install_hook.py
git commit -m "fix(kernel-install): actively enforce the boot-default pin, normalize marker content

Fedora's 90-loaderentry.install runs before this 99- hook and sets
saved_entry to the newly-installed stock kernel during the same
transaction (confirmed by tonight's incident: saved_entry ended up on
the new 7.2.6 stock entry). A pin-branch that merely returned early
instead of writing the pin back left that stock value in place."
```

---

## Task 7: Desktop shortcut — yad script, `.desktop` launcher, sudoers entry

**Files:**
- Create: `distros/fedora-installer/schema-boot-check.sh`
- Create: `distros/fedora-installer/schema-boot-check.desktop`
- Create: `distros/fedora-installer/schema-boot-check.sudoers`

**Interfaces:**
- Consumes: `schema-doctor --heal --json` (existing CLI, confirmed installed at `/usr/local/bin/schema-doctor` — re-verify this path on the actual target box before deploying, per the spec's note)
- Produces: nothing consumed by later tasks — this is a leaf deliverable, verified manually (no automated test; matches the existing `firstboot-flip-wizard.sh`, which also has no automated test, only on-box acceptance).

- [ ] **Step 1: Write `schema-boot-check.sudoers`**

```
# schema-boot-check (the desktop "check after update, before reboot" shortcut)
# runs unprivileged and reaches root ONLY through this one fixed command. No
# wildcards, no other arguments accepted.
%wheel ALL=(root) NOPASSWD: /usr/local/bin/schema-doctor --heal --json
```

- [ ] **Step 2: Write `schema-boot-check.sh`**

```bash
#!/bin/bash
# schema boot check — click after `dnf update`, before rebooting, to confirm
# the schema boot entries are intact. Runs the full schema-doctor suite
# (already fast, already running unattended every 10 min) and reports on
# just the boot-entry-integrity result. Privileged work crosses to root
# through the one sudoers-permitted command below — that's the whole
# privileged surface (see schema-boot-check.sudoers), matching
# firstboot-flip-wizard.sh's H() pattern.
set -u

TITLE="schema — boot check"
ICON=drive-harddisk

info() { yad --title="$TITLE" --window-icon="$ICON" --width=520 --borders=18 \
             --image="$1" --text="$2" --button="$3":0 "${@:4}" 2>/dev/null; }

report_json="$(sudo /usr/local/bin/schema-doctor --heal --json 2>/dev/null)"
if [ -z "$report_json" ]; then
    info dialog-error "<b>Couldn't run the boot check.</b>\n\nschema-doctor didn't return a report. \
Don't reboot until you've looked into this manually." "Close"
    exit 1
fi

mapfile -t fields < <(printf '%s' "$report_json" | python3 -c '
import json, sys
try:
    checks = json.load(sys.stdin)
except Exception:
    print("error")
    print("Could not parse schema-doctor output.")
    print("")
    sys.exit(0)
for c in checks:
    if c.get("name") == "boot-entry-integrity":
        print(c.get("state", "unknown"))
        print((c.get("detail", "") or "").replace(chr(10), " "))
        print((c.get("action", "") or "").replace(chr(10), " "))
        sys.exit(0)
print("missing")
print("boot-entry-integrity check not found in report")
print("")
')

state="${fields[0]:-error}"
detail="${fields[1]:-}"
action="${fields[2]:-}"

case "$state" in
    clean)
        info dialog-information "<b>Boot entries OK.</b>\n\nSafe to reboot." "Close"
        ;;
    healed)
        info dialog-warning "<b>Fixed the boot entries.</b>\n\n${action}\n\nSafe to reboot now." \
             "Close" --button="Details":1
        ;;
    reported)
        info dialog-error "<b>Could not fix — do not reboot.</b>\n\n${detail}" "Close"
        ;;
    *)
        info dialog-error "<b>Unexpected result.</b>\n\n${detail}\n\nDo not reboot until this is understood." "Close"
        ;;
esac
```

- [ ] **Step 3: Write `schema-boot-check.desktop`**

```
[Desktop Entry]
Type=Application
Name=Check schema boot
Comment=Run after updates, before rebooting — confirms the schema boot entries are intact
Exec=/usr/local/bin/schema-boot-check.sh
Icon=drive-harddisk
Terminal=false
```

- [ ] **Step 4: Shell-lint the new script**

Run: `bash -n distros/fedora-installer/schema-boot-check.sh && command -v shellcheck >/dev/null && shellcheck distros/fedora-installer/schema-boot-check.sh || true`
Expected: `bash -n` reports no syntax errors; `shellcheck` (if installed) reports no errors (warnings about `set -u` with parameter expansion in `info()`'s `"${@:4}"` are expected and fine — matches `firstboot-flip-wizard.sh`'s existing style).

- [ ] **Step 5: Commit**

```bash
git add distros/fedora-installer/schema-boot-check.sh distros/fedora-installer/schema-boot-check.desktop distros/fedora-installer/schema-boot-check.sudoers
git commit -m "feat(installer): desktop shortcut to check schema boot entries before rebooting"
```

---

## Task 8: `schema-vmtest` boot-test — seed tonight's exact corruption, confirm auto-heal

**Files:** none (verification task, no code changes)

**Interfaces:** none — this task consumes everything built in Tasks 1-6 as a black box (boot a VM, corrupt its entries, let the periodic svc heal them, reboot).

- [ ] **Step 1: Boot-test under `schema-vmtest`**

Invoke the `schema-vmtest` skill against this branch (`feat/schema-doctor-boot-entry-integrity`). Inside the VM, once schema-init has booted successfully at least once:

```bash
# seed tonight's exact corruption shape
sudo sh -c '
for f in /boot/loader/entries/schema-*.conf; do
    sed -i "s/ init=\S*//; s/ modprobe.blacklist=radeon//" "$f"
done
grub2-editenv - set saved_entry=$(ls /boot/loader/entries/*.conf | grep -v schema- | head -1 | xargs basename | sed "s/.conf$//")
'
# confirm the corruption took
grep options /boot/loader/entries/schema-*.conf
grub2-editenv list
```

- [ ] **Step 2: Trigger the heal and confirm it worked**

```bash
sudo /usr/local/bin/schema-doctor --heal
grep options /boot/loader/entries/schema-*.conf   # expect init= and modprobe.blacklist=radeon back
grub2-editenv list                                 # expect saved_entry back on a schema entry
```

Expected: every `schema-*.conf` has `init=` and (if the VM has a `kernel-cmdline.d` drop-in seeded) its extras restored; `saved_entry` points at a `schema-` entry again.

- [ ] **Step 3: Reboot the VM and confirm it comes up under schema-init**

```bash
sudo reboot
# after reboot:
ps -p 1 -o comm=   # expect: schema-init
```

Expected: `schema-init`, not `systemd` — proves the healed entry actually boots correctly, not just that the file content looks right.

- [ ] **Step 4: Confirm the periodic timer catches it unattended (no manual `--heal`)**

Repeat Step 1's corruption, then wait for the existing 10-minute periodic tick (or, for a faster test loop, temporarily run `schema-doctor --heal --periodic` by hand to simulate one tick) and re-check as in Step 2, without any manual intervention beyond seeding the corruption.

Expected: same result as Step 2, confirming the periodic supervisor — not just a manual `--heal` — closes this gap.

---

## Task 9: Roll out to Optiplex

**Files:** none (deployment task — copies files built in prior tasks to the live host)

**Interfaces:** none — final integration step.

- [ ] **Step 1: Deploy the updated files**

```bash
scp -i ~/.ssh/id_ed25519_agents scripts/schema-doctor.py ajax80@192.168.8.101:/tmp/schema-doctor.py
scp -i ~/.ssh/id_ed25519_agents distros/shared/kernel-install/99-schema-init.install ajax80@192.168.8.101:/tmp/99-schema-init.install
scp -i ~/.ssh/id_ed25519_agents distros/fedora-installer/schema-boot-check.sh ajax80@192.168.8.101:/tmp/schema-boot-check.sh
scp -i ~/.ssh/id_ed25519_agents distros/fedora-installer/schema-boot-check.desktop ajax80@192.168.8.101:/tmp/schema-boot-check.desktop
scp -i ~/.ssh/id_ed25519_agents distros/fedora-installer/schema-boot-check.sudoers ajax80@192.168.8.101:/tmp/schema-boot-check.sudoers

ssh -i ~/.ssh/id_ed25519_agents ajax80@192.168.8.101 '
sudo install -m 0755 /tmp/schema-doctor.py /usr/local/bin/schema-doctor
sudo install -m 0755 /tmp/99-schema-init.install /usr/lib/kernel/install.d/99-schema-init.install
sudo install -m 0755 /tmp/schema-boot-check.sh /usr/local/bin/schema-boot-check.sh
sudo install -m 0440 /tmp/schema-boot-check.sudoers /etc/sudoers.d/schema-boot-check
sudo visudo -cf /etc/sudoers.d/schema-boot-check
install -m 0644 /tmp/schema-boot-check.desktop ~/Desktop/schema-boot-check.desktop
'
```

Note: confirm `/usr/lib/kernel/install.d/99-schema-init.install` is the real installed path for the hook on Optiplex before running this (re-verify per the spec's note — it may instead be under `/etc/kernel/install.d/`).

- [ ] **Step 2: Set the pin so `schema-ssd-7.1.12` survives future kernel updates**

```bash
ssh -i ~/.ssh/id_ed25519_agents ajax80@192.168.8.101 '
echo -n "schema-ssd-7.1.12-200.fc44.x86_64" | sudo tee /etc/schema-init/boot-default > /dev/null
cat /etc/schema-init/boot-default; echo
'
```

- [ ] **Step 3: Reload schema-init (never `restart`)**

```bash
ssh -i ~/.ssh/id_ed25519_agents ajax80@192.168.8.101 'sudo kill -HUP 1'
```

- [ ] **Step 4: On-box acceptance — corrupt by hand, confirm the desktop shortcut heals it**

```bash
ssh -i ~/.ssh/id_ed25519_agents ajax80@192.168.8.101 '
sudo sh -c "sed -i \"s/ init=\S*//; s/ modprobe.blacklist=radeon//\" /boot/loader/entries/schema-7.1.10-200.fc44.x86_64.conf"
grep options /boot/loader/entries/schema-7.1.10-200.fc44.x86_64.conf
'
```

Then click the `schema-boot-check` icon on Optiplex's desktop, confirm the amber "fixed" dialog appears, and re-check:

```bash
ssh -i ~/.ssh/id_ed25519_agents ajax80@192.168.8.101 'grep options /boot/loader/entries/schema-7.1.10-200.fc44.x86_64.conf'
```

Expected: `init=/sbin/schema-init` and `modprobe.blacklist=radeon` both restored.

- [ ] **Step 5: Confirm a real reboot still boots correctly**

```bash
ssh -i ~/.ssh/id_ed25519_agents ajax80@192.168.8.101 'sudo reboot'
# wait, then:
ssh -i ~/.ssh/id_ed25519_agents ajax80@192.168.8.101 'ps -p 1 -o comm=; cat /proc/cmdline'
```

Expected: `schema-init`, cmdline shows `init=/sbin/schema-init` and `modprobe.blacklist=radeon`.

---

## Self-Review Notes

- **Spec coverage:** `detect()`/`heal()` for both entry-token loss and saved_entry mis-pointing (Tasks 1-5); pin enforcement + marker normalization in the hook (Task 6); desktop shortcut with sudoers delegation (Task 7); vmtest boot-proof (Task 8); Optiplex rollout including the deploy-note pin (Task 9). All six second-opinion-review fixes (pin enforcement, cmdline.d extras, dangling saved_entry, duplicate init=, marker normalization, binary-path verification note) each have a task and a test.
- **Type consistency:** `BootEntryIntegrity` name, `_entries()`, `_options_line()`, `_missing_tokens()`, `_rewrite_options()`, `_read_saved_entry()`/`_set_saved_entry()`, `_pin_target()`/`_newest_entry()`, `_normalize_pin()` are each defined once (Tasks 1/2/3/4) and reused verbatim in every later task — no renames across tasks.
- **No placeholders:** every step has real, complete code — no "add error handling" or "similar to Task N" shortcuts.
