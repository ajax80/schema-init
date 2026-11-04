# podman exec scopes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `podman exec` into a schema-user Quadlet container works from a login shell, because podman's own `podman-*.scope` request is now honoured.

**Architecture:** The session shim (user) validates podman's `StartTransientUnit` scope request, mkdirs the scope under `user@U.service/user.slice/`, and calls system-bus `AttachProcessesToUnit`. The root bridge re-validates everything (uid, path, pid ownership, pidfd liveness) and writes the pid into `cgroup.procs`.

**Tech Stack:** Python 3 + dbus-python + GLib (existing scripts), plain-assert standalone tests (`make` runs every `tests/test_*.py`).

**Spec:** `docs/superpowers/specs/2026-11-03-podman-exec-scopes-design.md` (rev 2)

## Global Constraints

- Scope name regex: `^podman-[A-Za-z0-9_-]{1,64}\.scope$`. Subcgroup regex (bridge): `^/user\.slice/podman-[A-Za-z0-9_-]{1,64}\.scope$`.
- Cgroup root: `os.environ.get('SCHEMA_CGROUP_ROOT', '/sys/fs/cgroup')`; user tree `<root>/user.slice/user-U.slice/user@U.service`.
- Bridge: refuse caller uid 0; `unit == "user@%d.service" % C`; 1–4 pids; Uid all four fields == C; Gid NOT checked; never move a pid anywhere but the target.
- Shim: PIDs must be exactly `[sender pid]`; `Slice` absent or `user.slice`; ExecStart requests keep the existing spawn path; every other ExecStart-less request keeps the existing `NotSupported` refusal byte-for-byte.
- Shim → bridge call timeout 2 s; failure → rmdir scope, `NotSupported`.
- Sweep: on every accepted request + GLib 60 s timer; rmdir only `podman-*.scope` with `populated 0`.
- Errors: identity → `org.freedesktop.DBus.Error.AccessDenied`; shape → `org.freedesktop.DBus.Error.InvalidArgs`.
- No docstrings; why-comments only.

## Review Focus

- A scope request while `user.slice` holds a non-podman child or processes (hand-written `name=user.slice` .svc) → refused, nothing created. Test in Task 2.
- Bridge called with a pid that exits between check and write → error, no write anywhere else. Test in Task 1 (pidfd readable before write).
- Shim with no system bus / bridge missing the method (older package) → podman unaffected (NotSupported). Test in Task 2 (bridge call raises).
- Leftover populated scope dir with the same name (pid reuse while a detached conmon lives there) → refused, not reused. Test in Task 2.
- `SCHEMA_CGROUP_ROOT` fake tree with `user.slice` as a symlink → bridge refuses. Test in Task 1.

---

### Task 1: Bridge `AttachProcessesToUnit`

**Files:**
- Modify: `scripts/schema-systemd1.py` (helpers near the top after `svc_name`; method on `Systemd1Manager` after `StartTransientUnit`)
- Create: `tests/test_systemd1_attach.py`

**Interfaces:**
- Produces: `attach_validate(caller_uid: int, unit: str, subcgroup: str, pids: list[int], cg_root: str) -> str` returns the absolute target dir or raises `AttachRefused(kind, why)` where kind ∈ {"AccessDenied", "InvalidArgs"}.
- Produces: `attach_pids(target: str, pids: list[int], caller_uid: int) -> None`, raising `AttachRefused` on any failure. Per pid: `os.pidfd_open` → Uid line check → `select.poll` on fd shows not readable → write pid to `target/cgroup.procs` (ESRCH → refuse) → poll again; readable → refuse ("exited during attach").
- Produces: D-Bus method `AttachProcessesToUnit(s, s, au)` on `org.freedesktop.systemd1.Manager`, `sender_keyword='sender'`, caller uid via `self.bus.get_unix_user(sender)`; maps `AttachRefused` to `dbus.exceptions.DBusException(why, name='org.freedesktop.DBus.Error.' + kind)`; logs one line per call.

- [ ] **Step 1: Write failing tests** in `tests/test_systemd1_attach.py` (standalone, `check(name, ok)` style as `test_systemd1_reload_dbus.py`). Build a fake root in a tempdir: `user.slice/user-<me>.slice/user@<me>.service/user.slice/podman-1.scope/` with an empty `cgroup.procs` file. Cases (me = `os.getuid()`, which is non-zero in CI):
  - valid → returns target dir path ending `/user.slice/podman-1.scope`.
  - caller 0 → AccessDenied; `unit="user@%d.service" % (me+1)` → AccessDenied.
  - subcgroups `/user.slice/../x`, `/foo.scope`, `/user.slice/app-1.scope`, `/user.slice/podman-a/b.scope` → InvalidArgs.
  - 0 pids, 5 pids → InvalidArgs.
  - leaf missing → InvalidArgs; `user.slice` replaced by a symlink to another dir holding `podman-1.scope` → AccessDenied.
  - `attach_pids(target, [os.getpid()], me)` → `cgroup.procs` contains `str(os.getpid())`.
  - `attach_pids(target, [1], me)` → AccessDenied (pid 1 is root), file untouched.
  - exited pid: spawn `subprocess.Popen(['true'])`, `wait()` it (zombie gone) → refused (ESRCH at pidfd_open), file untouched.
  - poll-before race: monkeypatch `m._exited = lambda fd: True` → refused, file untouched.
  - poll-after race: `_exited` returns False then True → refused, and the file holds only that pid (nothing written elsewhere).
- [ ] **Step 2: Run** `python3 tests/test_systemd1_attach.py` → fails (no `attach_validate`).
- [ ] **Step 3: Implement** `AttachRefused(Exception)` with `.kind`, `attach_validate`, `_exited(fd) -> bool` (poll `POLLIN`, timeout 0), `_pid_uids(pid) -> tuple[int,int,int,int]` (parse `Uid:` of `/proc/<pid>/status`), `attach_pids`, and the D-Bus method. Path checks: lstat `user@C.service`, `.../user.slice`, leaf: `S_ISDIR` and not `S_ISLNK` and `st_uid == C`; then `commonpath([user_svc, realpath(target)]) == user_svc`. Close every pidfd.
- [ ] **Step 4: Run** `python3 tests/test_systemd1_attach.py` → all PASS; `python3 tests/test_systemd1_reload_dbus.py` still PASS.
- [ ] **Step 5: Commit** `systemd1: AttachProcessesToUnit for user podman scopes`.

### Task 2: Shim accepts podman scope requests

**Files:**
- Modify: `scripts/schema-systemd1-session.py` (`handle_start_transient_unit`, new helpers, `main` timer)
- Modify: `tests/test_systemd1_session.py` (new section)

**Interfaces:**
- Consumes: bridge method `AttachProcessesToUnit("user@U.service", "/user.slice/<name>", [pid])` from Task 1.
- Produces: `_scope_check(name: str, props, sender_pid: int) -> str | None` (None = acceptable, else why). Reads `PIDs` (`au`) and `Slice` (`s`) from `props` (`a(sv)`).
- Produces: `_scope_prepare(name: str, uid: int, cg_root: str) -> str` returns scope dir, raises `ScopeRefused(why)`. Checks `user@U.service` owned by uid; `user.slice` (mkdir if absent) has empty `cgroup.procs` and only `podman-*.scope` children; leaf mkdir, or reuse if it exists and `cgroup.events` says `populated 0`, else refuse.
- Produces: `_scope_sweep(uid: int, cg_root: str) -> int` (number removed).
- Produces: `_sender_pid(session, sender) -> int` via `org.freedesktop.DBus.GetConnectionUnixProcessID`.
- `attach` is injected (`_attach_fn(system, uid, name, pid)`) so tests can stub it.

- [ ] **Step 1: Write failing tests** (append to `main()` in `tests/test_systemd1_session.py`):
  - `_scope_check("podman-42.scope", [("Slice","user.slice"),("PIDs",[42]),("Delegate",True),("DefaultDependencies",False)], 42) is None`.
  - not None for: `app-x.scope`, `chrome-1.scope`, PIDs `[43]`, PIDs `[42,43]`, no PIDs, `Slice=foo.slice`.
  - Fake root tempdir (`user.slice/user-<me>.slice/user@<me>.service/`): `_scope_prepare` creates `user.slice/podman-42.scope`; a second call with the dir present and `cgroup.events` = `populated 0\n` reuses it; with `populated 1\n` → `ScopeRefused`.
  - `user.slice/web/` (non-podman child) present → `ScopeRefused`, no leaf created. `user.slice/cgroup.procs` containing `123\n` → `ScopeRefused`.
  - `_scope_sweep`: `podman-1.scope` (populated 0) removed, `podman-2.scope` (populated 1) kept, returns 1. A non-podman dir is never touched (by construction it can't be present; assert sweep ignores `other.scope` placed next to them).
  - Fake-root note: on a plain fs rmdir fails if the dir holds files. Sweep must unlink the fake files only when `cg_root != '/sys/fs/cgroup'`? **No** — instead the tests create scope dirs with no files and fake `populated` through a monkeypatched `m._populated(path) -> bool`. Real cgroupfs dirs always hold kernel files and rmdir works on them.
  - handler path: build the request through a stub `attach` that raises → scope dir removed, reply is the NotSupported error. Stub that succeeds → `_reply_job_and_done` called with the name.
- [ ] **Step 2: Run** `python3 tests/test_systemd1_session.py` → fails.
- [ ] **Step 3: Implement.** In `handle_start_transient_unit`, when there is no ExecStart: if `_SCOPE_RE` matches the name, try `_scope_check` → `_scope_prepare` → `_scope_sweep` → attach (system bus `call_blocking`-style proxy call with `timeout=2`); success → `_reply_job_and_done`; any failure → rmdir the leaf if we created it and send the existing NotSupported error. Log `scope <name> pid=<pid> -> attached` / `-> refused (<why>)`. Non-matching names fall through to the existing refusal untouched. In `main`, `GLib.timeout_add_seconds(60, ...)` the sweep (return True). The user's uid is `os.getuid()`.
- [ ] **Step 4: Run** both `python3 tests/test_systemd1_session.py` and `make check` (or the Makefile python loop target) → PASS.
- [ ] **Step 5: Commit** `systemd1-session: honour podman's own scope request`.

### Task 3: Live script, e2e, docs

**Files:**
- Create: `tests/livetest/podman-exec-scope.sh`
- Modify: `tests/livetest/podman-quadlet-e2e.sh:24-29` (drop the e2e-hog .svc; run the hog with `podman exec` from the shell; check 6 unchanged)
- Modify: `docs/svc-reference.md` (Quadlet/rootless section: one paragraph) and `docs/roadmap.md` (tick)

- [ ] **Step 1: Write** `podman-exec-scope.sh` (same `pass`/`fail` helpers as e2e). It writes a `scopetest.container` (debian:trixie, `sleep infinity`, `[Install] WantedBy=default.target`), runs `systemctl --user daemon-reload`, `schema-ctl --user reload`, and waits for it to be running. Checks, numbered as spec Tests 1–7: exec rc=0; scope dir present during `podman exec … sleep 3` and gone ≤ 60 s later; `busctl --user call org.freedesktop.systemd1 /org/freedesktop/systemd1 org.freedesktop.systemd1.Manager StartTransientUnit 'ssa(sv)a(sa(sv))' chrome-1.scope fail 0 0` fails with NotSupported; system-bus `AttachProcessesToUnit` with pid 1 / `user@0.service` / `/user.slice/../..` gives AccessDenied/AccessDenied/InvalidArgs; no `scope podman-` lines in `schema-stu.log` attributed to the Quadlet start window; ad-hoc `podman run --rm` + `-d` both work, printing `t`'s conmon cgroup; `time podman ps` printed. Cleanup in a trap: rm the quadlet, reload `--evict`, `podman rm -f t`.
- [ ] **Step 2:** `bash -n` both scripts; `shellcheck` if present.
- [ ] **Step 3:** docs paragraphs.
- [ ] **Step 4: Commit** `livetest: podman exec scope check; e2e execs directly`.

### Task 4: Deploy-time verification (blakbox, after merge + COPR)

- [ ] `sudo dnf upgrade 'schema-init*'`; restart the bridge (`sudo schema-ctl restart schema-systemd1`; it is not a session daemon); kill the session shim so the bus re-activates the new one (`pkill -f schema-systemd1-session`; it is bus-activated and stateless).
- [ ] Run `tests/livetest/podman-exec-scope.sh` → all PASS; mutation check (spec Test 8) by hand.
- [ ] Open Helium, load two pages (Chromium path unchanged).
