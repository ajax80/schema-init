# schema-logind Single Session Writer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `schema-session-register`/`-unregister` the single writer of record for logind sessions; `CreateSession`/`ReleaseSession` and the GUI autologin all bottom out there, and legacy `sddm-logged` is retired.

**Architecture:** `CreateSession` keeps its VT-only guard and existing-session reuse check, then execs `schema-session-register` (inheriting its `SCHEMA_LOGIND_RUN_DIR`/`SCHEMA_CGROUP_ROOT` env) instead of writing the state file and scope itself; `ReleaseSession` execs `schema-session-unregister`. The GUI login unifies on `schema-plasma-autologin.sh` (already self-parameterizing), and `sddm-logged` is removed.

**Tech Stack:** Python 3 (`dbus-python`, GLib main loop), POSIX shell, schema-init `.svc` service files, custom test runners (not pytest), `schema-vmtest` (QEMU/KVM).

**Spec:** `docs/superpowers/specs/2026-08-26-logind-single-writer-design.md`

## Global Constraints

- **Never block a login.** Every session write/teardown path falls through to the legacy id and exits 0 on any failure. Copied verbatim from spec §A.
- **Single writer:** only `schema-session-register`/`-unregister` may write `/run/systemd/sessions/<id>` or `session-<id>.scope`. Spec §A.
- **Test runners are custom scripts**, invoked as `./tests/<name>.py` (exit 0 = all pass, 1 = any fail). They assert via `check(name, ok, detail)`, never pytest asserts.
- **Rollout order is hard:** `schema-vmtest` → eli → blakbox. blakbox is the daily driver / fleet brain and is never first. Spec §E.
- **Deploy schema-init via `kill -HUP`, never `restart`** (project rule). Reboots on eli/blakbox are performed by Jonathan, not the agent.
- **`pgrep -x`, not `pkill -f`; `printf`, not heredoc-in-shell where a printf will do** (project idiom).

---

### Task 1: `CreateSession` delegates the write to `schema-session-register`

**Files:**
- Modify: `scripts/schema-logind.py` (the `CreateSession` method, ~lines 1803–1891)
- Test: `tests/test_logind_create_session.py`

**Interfaces:**
- Consumes: `schema-session-register` CLI (`--uid --user --seat --vtnr --type --class --desktop --service --leader`, prints sid on stdout, honors `SCHEMA_LOGIND_RUN_DIR`/`SCHEMA_CGROUP_ROOT` env).
- Produces: unchanged `CreateSession` D-Bus contract — returns `(sid, object_path, runtime_path, fifo_fd, uid, seat, vtnr, existing)`.

- [ ] **Step 1: Write the failing parity test.** Append to `tests/test_logind_create_session.py`, inside the main test body after the existing "a tty login gets a session" block (it already has `create`, `cgroot`, `rundir`, `check` in scope):

```python
        print("\n-- CreateSession writes the same file schema-session-register would --")
        # Drive the helper directly into a throwaway run dir with the SAME args
        # CreateSession maps, then compare the resulting state file key-for-key
        # (except the two timestamps, which are wall-clock and always differ).
        import glob
        ref_run = tempfile.mkdtemp(prefix='ref-run-')
        ref_cg = tempfile.mkdtemp(prefix='ref-cg-')
        helper = os.path.join(REPO, 'scripts', 'schema-session-register')
        henv = dict(os.environ)
        henv['SCHEMA_LOGIND_RUN_DIR'] = ref_run
        henv['SCHEMA_CGROUP_ROOT'] = ref_cg
        henv['SCHEMA_LOGIND_ACTIVE_VT'] = os.environ.get('SCHEMA_LOGIND_ACTIVE_VT', '')
        ref_sid = subprocess.check_output(
            [helper, '--uid', str(uid), '--user', str(uid), '--seat', 'seat0',
             '--vtnr', '2', '--type', 'tty', '--class', 'user',
             '--service', 'login', '--leader', str(leader.pid)],
            env=henv, text=True).strip()

        def stable_keys(path):
            d = {}
            for line in open(path):
                if '=' in line and not line.startswith('#'):
                    k, v = line.rstrip('\n').split('=', 1)
                    if k not in ('REALTIME', 'MONOTONIC'):
                        d[k] = v
            return d

        cs_file = os.path.join(rundir, 'sessions', sid)      # written by CreateSession above
        ref_file = os.path.join(ref_run, 'sessions', ref_sid)
        check('CreateSession state file matches the helper key-for-key',
              stable_keys(cs_file) == stable_keys(ref_file),
              '%s vs %s' % (stable_keys(cs_file), stable_keys(ref_file)))
        shutil.rmtree(ref_run, ignore_errors=True)
        shutil.rmtree(ref_cg, ignore_errors=True)
```

- [ ] **Step 2: Run the suite to verify the new check FAILS.**

Run: `./tests/test_logind_create_session.py`
Expected: FAIL on `CreateSession state file matches the helper key-for-key` — CreateSession currently writes `SERVICE=login` and computes fields via its own `write_session_file`, and differences (e.g. `USER` resolution, `IS_DISPLAY`, key order-independent values) surface here. (If it happens to pass, the delegation is trivial; proceed anyway — the point is to lock the parity.)

- [ ] **Step 3: Replace the write body with a helper exec.** In `scripts/schema-logind.py`, inside `CreateSession`, keep the VT guard (the `vtnr <= 0` block) and the existing-VT-session reuse loop unchanged. Replace everything from `sid = alloc_session_id()` through the `os.makedirs(scope)` / `cgroup.procs` block (down to just before the final `print(... CreateSession -> ...)`) with:

```python
        username = get_username_for_uid(uid)
        cmd = [SESSION_REGISTER,
               '--uid', str(uid), '--user', str(username or uid),
               '--seat', seat_id, '--vtnr', str(vtnr),
               '--type', str(type_) if str(type_) not in ('', 'unspecified') else 'tty',
               '--class', str(class_) or 'user',
               '--desktop', str(desktop),
               '--service', str(service) or 'login',
               '--leader', str(pid)]
        if str(desktop):  # a DM login is a display session
            cmd.append('--display')
        # The daemon's own env already carries SCHEMA_LOGIND_RUN_DIR /
        # SCHEMA_CGROUP_ROOT (real or test), so inheriting it is what makes the
        # helper write to the same tree the registry reads. Never block a login:
        # a helper failure still returns a reply on the legacy id.
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
            sid = (out.stdout.strip().splitlines() or [''])[-1]
        except Exception as e:
            print("login1-stub: schema-session-register failed: %s" % e,
                  file=sys.stderr)
            sid = ''
        if not sid.isdigit():
            sid = LEGACY_SESSION_ID
```

Add near the top of the file, beside the other path constants (e.g. after `LEGACY_SESSION_ID = '31'`):

```python
SESSION_REGISTER = os.environ.get(
    'SCHEMA_SESSION_REGISTER',
    os.path.join(os.path.dirname(os.path.abspath(__file__)), 'schema-session-register'))
```

Ensure `import subprocess` is present at the top (it already is — used elsewhere).

- [ ] **Step 4: Run the suite to verify PASS.**

Run: `./tests/test_logind_create_session.py`
Expected: PASS — the new parity check passes AND every pre-existing check (returns id/paths/fifo, Leader is the pid, scope created, leader in scope, sudo/su/pts refused, vtnr recovered, reuse flagged existing) stays green. Those existing checks are the refactor's regression guard.

- [ ] **Step 5: Commit.**

```bash
git add scripts/schema-logind.py tests/test_logind_create_session.py
git commit -m "feat(logind): CreateSession delegates session write to schema-session-register"
```

---

### Task 2: Remove the now-dead Python writers

**Files:**
- Modify: `scripts/schema-logind.py` (delete `write_session_file` and the Python `alloc_session_id` definitions)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing (pure deletion of unreferenced code).

- [ ] **Step 1: Confirm zero remaining callers.**

Run: `grep -nE "write_session_file|alloc_session_id" scripts/schema-logind.py`
Expected: only the `def write_session_file` and `def alloc_session_id` lines themselves remain (the CreateSession call sites are gone after Task 1). If any other caller appears, STOP and report — do not delete.

- [ ] **Step 2: Delete both function definitions** (`def write_session_file(...)` at ~line 531 and its body; `def alloc_session_id(...)` and its body). Leave the shell helper's own `alloc_session_id` untouched — it is a different file.

- [ ] **Step 3: Run the full logind test set to verify nothing regressed.**

Run: `./tests/test_logind_create_session.py && ./tests/test_logind_registry.py && ./tests/test_logind_session_alloc.py && ./tests/test_logind_multisession.py`
Expected: all exit 0.

- [ ] **Step 4: Commit.**

```bash
git add scripts/schema-logind.py
git commit -m "refactor(logind): drop the now-unused Python session-file writer and allocator"
```

---

### Task 3: `ReleaseSession` delegates teardown to `schema-session-unregister`

**Files:**
- Modify: `scripts/schema-logind.py` (the `ReleaseSession` method — inline `unlink`/`rmdir` around lines 1300–1330 / 1900–1930)
- Test: `tests/test_logind_create_session.py`

**Interfaces:**
- Consumes: `schema-session-unregister <sid> <uid>` (removes the state file + `rmdir`s the scope; honors the same env overrides).
- Produces: unchanged `ReleaseSession` D-Bus contract (void reply).

- [ ] **Step 1: Write the failing teardown test.** Append inside the main body, after a session has been created (reuse `sid`, `leader`, `rundir`, `cgroot`, `mgr` in scope):

```python
        print("\n-- ReleaseSession removes the state file and the scope --")
        rel_leader = subprocess.Popen(['sleep', '120'])
        rr = create(pid=rel_leader.pid, service='login', class_='user',
                    tty='tty7', vtnr=7)
        rsid = str(rr[0])
        time.sleep(SETTLE)
        rfile = os.path.join(rundir, 'sessions', rsid)
        rscope = os.path.join(cgroot, 'user.slice', 'user-%d.slice' % uid,
                              'session-%s.scope' % rsid)
        check('release: file present before', os.path.exists(rfile), rfile)
        rel_leader.terminate()  # free the scope so rmdir can succeed
        rel_leader.wait()
        mgr.ReleaseSession(rsid, dbus_interface=MANAGER_IFACE)
        time.sleep(SETTLE)
        check('release: state file removed', not os.path.exists(rfile), rfile)
        check('release: scope rmdir-ed', not os.path.isdir(rscope), rscope)
```

- [ ] **Step 2: Run to verify it FAILS** (or passes only incidentally on the file, not the scope).

Run: `./tests/test_logind_create_session.py`
Expected: FAIL on `release: scope rmdir-ed` — current `ReleaseSession` teardown path differs from the helper's `rmdir`-not-`rm -r` behavior.

- [ ] **Step 3: Delegate teardown.** In `ReleaseSession`, replace the inline `os.unlink(session_file...)` + `os.rmdir(...scope...)` with:

```python
        uid = self.registry.sessions[sid].record.uid if sid in self.registry.sessions else ''
        try:
            subprocess.run([SESSION_UNREGISTER, str(sid), str(uid)], timeout=5)
        except Exception as e:
            print("login1-stub: schema-session-unregister failed: %s" % e,
                  file=sys.stderr)
        self.registry.sync()
```

Add the constant beside `SESSION_REGISTER`:

```python
SESSION_UNREGISTER = os.environ.get(
    'SCHEMA_SESSION_UNREGISTER',
    os.path.join(os.path.dirname(os.path.abspath(__file__)), 'schema-session-unregister'))
```

- [ ] **Step 4: Run to verify PASS.**

Run: `./tests/test_logind_create_session.py`
Expected: PASS — the two release checks pass and all prior checks stay green.

- [ ] **Step 5: Commit.**

```bash
git add scripts/schema-logind.py tests/test_logind_create_session.py
git commit -m "feat(logind): ReleaseSession delegates teardown to schema-session-unregister"
```

---

### Task 4: Unify the GUI login on `schema-plasma-autologin.sh`; retire `sddm-logged`

**Files:**
- Move/Create: canonical `distros/fedora-kde/scripts/schema-plasma-autologin.sh` (from `distros/fedora-installer/rail/scripts/schema-plasma-autologin.sh`)
- Delete: `distros/fedora-kde/scripts/sddm-logged`
- Modify: the fedora-kde service that launches the login (the `.svc` whose `ExecStart` runs `sddm-logged`)

**Interfaces:**
- Consumes: `schema-session-register`/`-unregister` (already called by the script).
- Produces: a single GUI-login entrypoint used by every distro profile.

- [ ] **Step 1: Locate the launcher that execs `sddm-logged`.**

Run: `grep -rnE "sddm-logged" distros/ services/ *.svc 2>/dev/null`
Record the `.svc` file and its `ExecStart` line. Expected: a fedora-kde autologin/plasma service under `distros/fedora-kde/services/`.

- [ ] **Step 2: Place the canonical script.** Copy the installer's self-parameterizing script into the fedora-kde profile so both profiles share one source:

```bash
git mv distros/fedora-installer/rail/scripts/schema-plasma-autologin.sh distros/fedora-kde/scripts/schema-plasma-autologin.sh
```

If the installer build references the old path, add a symlink or update the reference (grep first: `grep -rn "rail/scripts/schema-plasma-autologin.sh" distros/`). Update any hit to the new path.

- [ ] **Step 3: Repoint the launcher.** Edit the `.svc` from Step 1 so `ExecStart` runs `schema-plasma-autologin.sh` instead of `sddm-logged`. Match the deployed path the profile installs to (e.g. `/usr/local/bin/schema-plasma-autologin.sh`).

- [ ] **Step 4: Remove the retired script.**

```bash
git rm distros/fedora-kde/scripts/sddm-logged
```

- [ ] **Step 5: Syntax-check both scripts and the service.**

Run: `bash -n distros/fedora-kde/scripts/schema-plasma-autologin.sh && echo OK`
Run: `grep -n ExecStart distros/fedora-kde/services/*.svc | grep -i plasma`
Expected: `OK`, and the `ExecStart` shows the new script.

- [ ] **Step 6: Commit.**

```bash
git add -A distros/
git commit -m "feat(logind): unify GUI login on schema-plasma-autologin.sh; retire sddm-logged"
```

---

### Task 5: Validate under `schema-vmtest` (LIVE boot)

**Files:** none (validation only).

- [ ] **Step 1: Boot the branch under vmtest.** Invoke the `schema-vmtest` skill (or `scripts/` vmtest entrypoint) to boot this branch as PID 1 in LIVE mode.

- [ ] **Step 2: Assert a GUI session registered through the one writer.** In the booted VM (or its captured logs), confirm:
  - `loginctl list-sessions` shows exactly one seat session with a **non-empty `LEADER`** (not `-`).
  - `/run/systemd/sessions/<id>` exists with `LEADER=<pid>` and no hardcoded `31` unless `31` was the allocated id.
  - The polkit auth agent registered (a `GetNameOwner org.kde.polkit-kde-authentication-agent-1` returns an owner), i.e. `sd_pid_get_session` resolved.
- [ ] **Step 3: Run the doctor.** `schema-doctor --check` → **all CLEAN**, specifically the `session-single` check (no orphan `31`).
- [ ] **Step 4: Record the result** in the plan/PR. If any check fails, STOP and loop back — do not proceed to hardware.

---

### Task 6: Roll out to eli and verify (Jonathan reboots)

**Files:** none (deployment/validation).

- [ ] **Step 1: Deploy the branch to eli.** Sync `scripts/schema-logind.py`, `scripts/schema-session-register`, `scripts/schema-session-unregister`, and `schema-plasma-autologin.sh` to eli's deployed paths; install/repoint the autologin service. Do **not** reboot from the agent.
- [ ] **Step 2: Ask Jonathan to reboot eli.** State plainly that this is the first hardware test of the change.
- [ ] **Step 3: Verify on eli after reboot** (`ssh ajax80@192.168.8.213`):

```bash
loginctl list-sessions        # one session, real LEADER (not '-')
schema-doctor --check         # all CLEAN
```

Ask Jonathan to confirm with his eyes: a GUI privileged action (e.g. a settings change needing auth) **raises the password prompt** — proof the polkit agent registered natively.

- [ ] **Step 4: Gate.** eli must be fully green (real LEADER + doctor CLEAN + polkit prompt) before touching blakbox. If not, STOP and debug on eli.

---

### Task 7: Cut blakbox over (gated on eli green; Jonathan reboots)

**Files:** none on blakbox besides deployed artifacts.

- [ ] **Step 1: Back up the live launcher.**

```bash
sudo cp -a /usr/local/bin/sddm-logged /usr/local/bin/sddm-logged.bak-$(date +%Y%m%d-%H%M%S)
```

- [ ] **Step 2: Deploy** `schema-plasma-autologin.sh` to `/usr/local/bin/`, deploy the updated `schema-logind.py` + helpers, and repoint blakbox's login service `ExecStart` to the new script. Deploy schema-init changes via `kill -HUP` where applicable; do **not** reboot from the agent.
- [ ] **Step 3: Ask Jonathan to reboot blakbox** — the last, gated step. Remind him the back-out is: pick the previous boot, restore `sddm-logged.bak-*`, repoint the service, reboot.
- [ ] **Step 4: Verify on blakbox after reboot:**

```bash
loginctl list-sessions        # one session, real LEADER (not '-')
schema-doctor --check         # all CLEAN
```

Confirm the desktop came up normally and a GUI polkit action prompts.

- [ ] **Step 5: Open the PR** for `feat/logind-single-writer` → `master` once both machines are green, referencing the spec and the vmtest/eli/blakbox evidence.

---

## Self-Review

**Spec coverage:**
- §A single-writer contract → Tasks 1–3 (delegation) + Task 2 (remove duplicate writer). ✅
- §B CreateSession/ReleaseSession delegators → Tasks 1, 3. ✅
- §C retire sddm-logged / unify GUI login → Task 4. ✅
- §D teardown symmetry + doctor CLEAN → Task 3 + Tasks 5/6/7 doctor checks. ✅
- §E testing + staged rollout → Tasks 1/3 (TDD), Task 5 (vmtest), Tasks 6/7 (eli→blakbox). ✅
- §F risks (backup, never-block fallback) → Task 7 Step 1 backup; legacy-id fallback in Task 1 Step 3. ✅
- Non-goals (pidfd/power-key/uaccess) → intentionally absent. ✅

**Placeholder scan:** No TBD/TODO; every code step carries real code in the harness idiom. Task 4 Steps 1/2 use grep-first discovery (the launcher `.svc` name is environment-specific) with defined recorded output — an investigation step, not a placeholder.

**Type/name consistency:** `SESSION_REGISTER`/`SESSION_UNREGISTER` constants defined in Tasks 1/3 and used there; `schema-session-register` CLI flags match the helper's actual option parser; `check(name, ok, detail)` and `create(...)` match the existing test harness; `LEGACY_SESSION_ID` reused from the existing module constant.
