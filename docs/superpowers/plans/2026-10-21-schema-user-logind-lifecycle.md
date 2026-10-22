# schema-user lifecycle (PR C) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** schema-logind starts one `schema-user` per user in a delegated systemd-path cgroup when that user gains a session or lingers, restarts it with backoff, stops it 30 s after the last session closes, adopts running ones across its own re-exec, implements `SetUserLinger`; PID 1 SIGTERMs every `schema-user` at shutdown before the final kill.

**Architecture:** All lifecycle logic is a `UserSupervisors` class in `scripts/schema-logind.py`, driven from the existing 250 ms `SessionRegistry.sync()` tick — no new fds, threads or GLib sources (the CPU-spin rule in the `SessionRegistry` docstring). It spawns with `os.posix_spawn` (no `subprocess` bookkeeping racing our `waitpid`) through `sh -c` that moves itself into the delegated cgroup as root, then `exec setpriv` to the user. `schema-user` (PR B) already detects the delegation and moves into `init.scope`. PID 1 gets one more glob in its shutdown container-SIGTERM pass.

**Tech Stack:** Python 3 (schema-logind, GLib loop), C99 (`init.c`), util-linux `setpriv`.

**Spec:** `docs/superpowers/specs/2026-10-20-schema-user-supervisor-design.md` — §3 (cgroup delegation), §4 (lifecycle), Anti-goals (kill switch, login never waits). §5–§6 are PR D; §7 shim, §8, §9 PR E.

## Global Constraints

- Cgroup path `/sys/fs/cgroup/user.slice/user-<uid>.slice/user@<uid>.service/`. Delegation = chown the dir and its `cgroup.procs`, `cgroup.subtree_control`, `cgroup.threads` to the uid (gid = passwd gid); enable on `user.slice` and `user-<uid>.slice` the controllers listed in the root's `cgroup.subtree_control`.
- Kill switch: `/etc/schema-init/user-supervisor.disabled` exists, **or** `schema.user=0` is on the kernel command line → logind never starts `schema-user`. Instances already running are left alone.
- Login never waits on `schema-user`: every failure (cgroup, spawn, crash) is logged and swallowed; nothing in `CreateSession`/`sync` blocks on it.
- Grace 30 s after a uid's last session goes; lingering users are never stopped for lack of a session.
- Adopt, never kill-and-replace, on logind start/`kill -HUP` re-exec. logind's own SIGTERM leaves every `schema-user` running.
- Linger files `/var/lib/systemd/linger/<username>`.
- Backoff 1, 2, 4, 8, 16, 32, 60 s (cap 60); give up after 5 crashes within 300 s, logged once; a uid that leaves the wanted set and comes back starts fresh. Stop: SIGTERM, then after 20 s write `1` to `user@<uid>.service/cgroup.kill`.
- Never for uid 0, never for a uid with no passwd entry, never from a synthesised (legacy fallback) session record.
- `schema-user` log: `/var/log/schema-init/schema-user-<uid>.log`, opened append by logind, 0640.
- Test overrides via env, read once at import like the existing ones: `SCHEMA_CGROUP_ROOT` (exists), `SCHEMA_LINGER_DIR`, `SCHEMA_USER_BIN` (`/usr/bin/schema-user`), `SCHEMA_USER_KILLSWITCH`, `SCHEMA_LOGIND_CMDLINE` (`/proc/cmdline`), `SCHEMA_USER_LOG_DIR` (`/var/log/schema-init`).
- Python tests run unprivileged in `make test-all`. `make test` stays green. Why-comments only.

## Review Focus

1. **No real session, only the legacy synthesised record** (empty `/run/systemd/sessions`, e.g. early boot or a login script that never registers) → no `schema-user` starts. Test in Task 2.
2. **`kill -HUP` re-exec while a `schema-user` is running** → same pid afterwards, no second instance, no SIGTERM. Test in Task 2 (`adopt` on a fresh `UserSupervisors` over a live child).
3. **Autologin respawn inside the grace window** (session 3 closes, session 4 for the same uid appears 5 s later) → the instance is never signalled. Test in Task 2.
4. **`schema-user` binary missing or exits at once every time** (bad package, kill switch file absent but binary broken) → backoff, give-up after 5, one log line, desktop unaffected, no busy loop. Test in Task 2; VM in Task 5.
5. **A stranger's process named like `schema-user`** sitting in `user@<uid>.service` for a different uid, or a recycled pid → not adopted / not signalled: adoption reads the uid from the cgroup path and the start-ticks baseline guards every later signal. Test in Task 2.

---

### Task 1: Delegation and spawn helpers

**Files:**
- Modify: `scripts/schema-logind.py` (constants beside `CGROUP_ROOT` :270; new helpers after `get_gid_for_uid` :95; `main()` runtime-dir block :2790–2796 → `ensure_runtime_dir`)
- Test: `tests/test_logind_user_supervisor.py` (new; same harness as `tests/test_logind_registry.py`: env set, then `importlib` load, `check()` list, exit 1 on any FAIL)

**Interfaces:**
- Produces: `user_service_cgroup(uid: int) -> str` → `CGROUP_ROOT + '/user.slice/user-%d.slice/user@%d.service'`.
- Produces: `delegate_user_cgroup(uid: int) -> str | None` — makedirs (exist_ok), enables controllers on the two slices (each `+ctl` written separately, errors ignored), chowns the four paths to `uid:gid`; returns the path, or `None` with a logged reason.
- Produces: `remove_user_cgroup(uid: int) -> None` — rmdir every dir under and including `user@<uid>.service`, deepest first, errors ignored.
- Produces: `ensure_runtime_dir(uid: int) -> str` — `/run/user/<uid>` 0700 chowned, as `main()` does today (`main()` calls it for `get_active_uid()`).
- Produces: `spawn_user_supervisor(uid: int) -> int` (pid) — `os.posix_spawn('/bin/sh', argv, env, setsid=True, file_actions=[stdin /dev/null, stdout+stderr the log])`. Env: `HOME`, `USER`, `LOGNAME`, `SHELL` from passwd, `PATH=/usr/local/bin:/usr/bin:/bin`, `XDG_RUNTIME_DIR=/run/user/<uid>`. argv:
  - euid 0: `['/bin/sh', '-c', 'echo $$ > "$1" && { cd "$5" || cd /; } && exec setpriv --reuid="$2" --regid="$3" --init-groups -- "$4"', 'sh', <cgroup.procs>, uid, gid, SCHEMA_USER_BIN, home]`
  - otherwise (tests): same script without `setpriv --reuid… --`.
  The move happens as root before exec because moving a process needs write access to the common ancestor's `cgroup.procs`, which the user never has.
- Produces: `find_user_supervisors() -> dict[int, int]` (uid → pid) — glob `user.slice/user-*.slice/user@*.service/init.scope/cgroup.procs` and `…/user@*.service/cgroup.procs`; uid parsed from the `user@<uid>.service` component; keep a pid only if `/proc/<pid>/exe` minus ` (deleted)` equals `SCHEMA_USER_BIN` or `SCHEMA_USER_BIN` is `argv[0]` or `argv[1]` of `/proc/<pid>/cmdline` (the second form is what a test's shell-script stand-in looks like).

- [ ] **Step 1: Write tests** in `tests/test_logind_user_supervisor.py`. Temp tree as `SCHEMA_CGROUP_ROOT` with root `cgroup.subtree_control` = `cpu memory pids`; uid = `os.getuid()`. Since a temp dir does not grow kernel interface files, a fixture `fake_kernel_files(dir)` creates empty `cgroup.procs`, `cgroup.subtree_control`, `cgroup.threads`, `cgroup.controllers` in a dir before the call.
  - `test_cgroup_path`: `user_service_cgroup(1000)` endswith `/user.slice/user-1000.slice/user@1000.service`.
  - `test_delegate`: returns the path; dir exists; `user.slice/cgroup.subtree_control` and `user-<uid>.slice/cgroup.subtree_control` contain `+memory` (last write wins on a plain file — assert the file is non-empty and the last write is one of `+cpu +memory +pids`); the four paths `st_uid == uid`.
  - `test_delegate_unwritable`: root path is a file, not a dir → returns `None`, no exception.
  - `test_spawn_moves_then_execs`: `SCHEMA_USER_BIN` = temp script `#!/bin/sh` + `trap 'exit 0' TERM; while :; do sleep 0.1; done`; after `spawn_user_supervisor(uid)`, within 2 s `user@<uid>.service/cgroup.procs` contains the pid, `/proc/<pid>/cmdline` holds the script path, env `XDG_RUNTIME_DIR` (read `/proc/<pid>/environ`) is `/run/user/<uid>`; SIGTERM it, `waitpid` returns.
  - `test_find`: write that pid into `user@<uid>.service/init.scope/cgroup.procs` → `find_user_supervisors() == {uid: pid}`; write `os.getpid()` (python, not the bin) instead → `{}`.
  - `test_remove`: nested `init.scope/x/` dirs → all gone after `remove_user_cgroup(uid)`; `user-<uid>.slice` still exists.
- [ ] **Step 2: Run** `python3 tests/test_logind_user_supervisor.py` — expected FAIL (AttributeError on the helpers).
- [ ] **Step 3: Implement** the helpers and the constants; switch `main()` to `ensure_runtime_dir`.
- [ ] **Step 4: Run** the new test and `python3 tests/test_logind_registry.py tests/test_logind_create_session.py` (each separately) — expected all PASS.
- [ ] **Step 5: Commit** `logind: delegated user@<uid>.service cgroup and schema-user spawn helpers`

### Task 2: `UserSupervisors` lifecycle, wired into the tick

**Files:**
- Modify: `scripts/schema-logind.py` (new class after `SessionRegistry`; `SessionRegistry.__init__` gets `self.supervisors = None`; `sync()` calls it after `_sync_users()`; `main()` builds it, calls `adopt()` before the first `registry.sync(handoff)` and attaches it)
- Test: `tests/test_logind_user_supervisor.py`

**Interfaces:**
- Consumes: Task 1 helpers; `proc_starttime_ticks(pid)` (:207); `SessionRecord.synthesised`.
- Produces: `SessionRegistry.session_uids() -> set[int]` — uids of non-synthesised records only.
- Produces: `user_supervisor_disabled() -> bool` — kill-switch file exists, or token `schema.user=0` in the cmdline file (read per call; the file is tiny and a test needs to flip it).
- Produces: `linger_uids() -> set[int]` — passwd uid of each entry in `LINGER_DIR`, unknown names skipped (Task 3 fills `LINGER_DIR` use; define it here).
- Produces: `class UserSupervisors` with `adopt() -> None`, `reconcile(session_uids: set[int], now: float | None = None) -> None` (`now` defaults `time.monotonic()`), `pid(uid) -> int | None` (for tests and Task 3). Per-uid state: `pid`, `start_ticks`, `absent_since`, `term_at`, `crash_times` (list of `now`), `next_start`, `gave_up`, `stopping` (bool: we sent SIGTERM).
- Constants: `USER_GRACE_S = 30`, `USER_STOP_TIMEOUT_S = 20`, `USER_BACKOFF_S = (1, 2, 4, 8, 16, 32, 60)`, `USER_CRASH_LIMIT = 5`, `USER_CRASH_WINDOW_S = 300`.

Liveness each tick, per tracked pid: `os.waitpid(pid, WNOHANG)` → returned pid means dead (our child, now reaped); `ChildProcessError` → alive iff `proc_starttime_ticks(pid) == start_ticks` (adopted / recycled-pid guard). Before every signal, re-check the start ticks.

`reconcile` order per uid in `wanted = (session_uids | linger_uids()) - {0}` ∪ tracked:
1. Dead and `stopping` → `remove_user_cgroup`, forget the uid.
2. Dead and not `stopping` → crash: append to `crash_times`, drop entries older than the window; ≥ `USER_CRASH_LIMIT` → `gave_up`, log `schema-user uid=<u> crashed <n> times in 300 s — giving up until next login`; else `next_start = now + USER_BACKOFF_S[min(len-1, 6)]`, log the delay.
3. In wanted: clear `absent_since`; if no pid, not `gave_up`, `now >= next_start`, not disabled, has passwd → `ensure_runtime_dir`, `delegate_user_cgroup` (None → still spawn: schema-user runs without per-service cgroups), `spawn_user_supervisor`, record `start_ticks`. Spawn `OSError` counts as a crash.
4. Not in wanted, running, not `stopping`: set `absent_since` once; `now - absent_since >= USER_GRACE_S` → SIGTERM, `stopping`, `term_at = now`.
5. `stopping` and `now - term_at >= USER_STOP_TIMEOUT_S` → write `1` to `cgroup.kill` (once).
6. Not in wanted and not running → forget the uid (so `gave_up` resets on a later login).

- [ ] **Step 1: Write tests** (fake clock = explicit `now`; `SCHEMA_USER_BIN` stand-ins: the Task 1 sleeper, and a `crasher` script `exit 3`; kill switch / cmdline / linger dir point into the temp tree). Helper `settle(sv, uids, now)` calls `reconcile` then sleeps 0.2 s so children can run/exit.
  - `test_start_on_session`: `reconcile({uid}, 0)` → `pid(uid)` is a live sleeper.
  - `test_session_uids_skips_synthesised`: a `SessionRegistry.__new__` instance with `.sessions` = `{'31': stub(record=SessionRecord('31', None, synthesised=True)), '4': stub(record=SessionRecord('4', {'UID': str(uid)}))}` → `session_uids() == {uid}`; with only the synthesised one → `set()`.
  - `test_kill_switch_file` / `test_kill_switch_cmdline` (`quiet schema.user=0 rw`): no start; `schema.user=01` does not count.
  - `test_grace`: running from t=0; `reconcile(set(), 10)`, `reconcile({uid}, 30)` (back inside the window) → same pid, never signalled. Then `reconcile(set(), 40)`, `reconcile(set(), 69)` → alive; `reconcile(set(), 70)` → SIGTERM sent; after it exits the next `reconcile` forgets the uid and `user@<uid>.service` is gone.
  - `test_stop_timeout_kill`: sleeper variant that ignores TERM (`trap '' TERM`); after SIGTERM at t, `reconcile(set(), t+20)` → `cgroup.kill` contains `1`. (Kill it yourself afterwards; the fake `cgroup.kill` is a plain file.)
  - `test_backoff_and_give_up`: crasher; drive `reconcile({uid}, t)` with `t` stepping past each `next_start`; assert gaps 1, 2, 4, 8 between spawns and no 6th spawn after 5 crashes inside 300 s; `reconcile(set(), …)` then `reconcile({uid}, …)` spawns again.
  - `test_adopt_after_reexec`: spawn via a first `UserSupervisors`, put the pid in `init.scope/cgroup.procs`; a second `UserSupervisors().adopt()` then `reconcile({uid}, 0)` → `pid(uid)` is the same pid, no new spawn (count processes running the bin = 1).
  - `test_adopt_wrong_uid`: pid sitting under `user@<uid+1>.service` → adopted as `uid+1` only, and `reconcile({uid}, 0)` starts a separate one for `uid` (it is not mistaken for `uid`'s).
  - `test_linger_without_session`: `LINGER_DIR/<own username>` exists → `reconcile(set(), 0)` starts it; `reconcile(set(), 100)` never stops it.
- [ ] **Step 2: Run** — expected FAIL (no `UserSupervisors`).
- [ ] **Step 3: Implement** the class, `session_uids`, `user_supervisor_disabled`, `linger_uids`, and the wiring in `sync()`/`main()`. `sync()` wraps the `reconcile` call in `try/except Exception` with a logged message: a bug here must never stop the session projection.
- [ ] **Step 4: Run** the new test plus all `tests/test_logind_*.py` — expected PASS.
- [ ] **Step 5: Commit** `logind: start, supervise, stop and adopt schema-user per user`

### Task 3: `SetUserLinger` for real

**Files:**
- Modify: `scripts/schema-logind.py` (`SetUserLinger` :1832, `Login1User.GetAll` :1655)
- Test: `tests/test_logind_user_supervisor.py`

**Interfaces:**
- Consumes: `LINGER_DIR`, `linger_uids()`, `UserSupervisors.reconcile`.
- Produces: `set_linger(uid: int, enable: bool) -> None` — create (empty, 0644) / unlink `LINGER_DIR/<username>`, makedirs the dir 0755; unknown uid raises `ValueError`.
- `SetUserLinger(uid, enable, interactive)` gets `sender_keyword='sender'`; target `0xFFFFFFFF` means the caller (systemd's convention for an unset uid); target 0 is refused (no `schema-user` for root); allowed for caller uid 0, or caller uid == target while that uid has a session in the registry; else `org.freedesktop.DBus.Error.AccessDenied`. Then `set_linger`, then `self.registry.sync()` so start/stop follows at once.
- `Login1User.GetAll` adds `'Linger': dbus.Boolean(uid in linger_uids())`.

- [ ] **Step 1: Write tests**: `test_set_linger_roundtrip` (file appears/disappears, `linger_uids()` follows); `test_set_linger_unknown_uid` raises `ValueError`; `test_linger_auth` — call the auth helper you factor out, `linger_allowed(caller: int | None, target: int, session_uids: set[int]) -> int | None` (returns the resolved target uid or None): `(0, 1000, ∅) → 1000`, `(1000, 1000, {1000}) → 1000`, `(1000, 0xFFFFFFFF, {1000}) → 1000`, `(0, 0, ∅) → None`, `(1000, 1001, {1000,1001}) → None`, `(1000, 1000, ∅) → None`, `(None, 1000, {1000}) → None`.
- [ ] **Step 2: Run** — expected FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all `tests/test_logind_*.py` — PASS.
- [ ] **Step 5: Commit** `logind: SetUserLinger and the Linger property`

### Task 4: PID 1 — SIGTERM each `schema-user` at shutdown

**Files:**
- Modify: `init.c` (`CONTAINER_CGROUP_PROC_GLOBS` :1339, its comment, the `shut_log` text in `container_cgroups_signal` :1370)

Only `init.scope` is signalled, never the whole `user@<uid>.service`: `schema-user` stops its services in reverse dependency order on SIGTERM, which a broadcast to every process would defeat. It is signalled in the same pass as containers and counted by the same `SHUT_GRACE_MS` wait loop (`init.c:1793`); what it has not finished by then gets the `kill(-1)` backstop. Nothing at runtime needs changing: PID 1's eviction and orphan sweeps act only on `services[]` (`engine.c:2278`), never on cgroups outside `schema-init/`.

- [ ] **Step 1:** Add `"/sys/fs/cgroup/user.slice/user-*.slice/user@*.service/init.scope/cgroup.procs"` to the list (not to `container_cgroups_kill`'s `cgroup.kill` globs); log text `"container/user-supervisor SIGTERM sent"`; extend the block comment with the why above in two lines.
- [ ] **Step 2: Run** `make test` — PASS, `make schema-init` warnings unchanged (only `init.c:133`). Behaviour is proved in Task 5's VM shutdown.
- [ ] **Step 3: Commit** `init: SIGTERM per-user supervisors with the containers at shutdown`

### Task 5: Docs, VM proof, blakbox, draft PR

**Files:**
- Modify: `docs/operations.md` § "Per-user supervisor (schema-user)" :250–271 — status becomes "logind starts it at your first session, stops it 30 s after your last one closes unless you linger (`loginctl enable-linger`)"; add rows for the cgroup path and `/var/log/schema-init/schema-user-<uid>.log`; kill switch paragraph (file + `schema.user=0` at the boot menu); drop the "needs the delegation logind will set up" wording from the no-delegation bullet (keep the bullet: hand runs still lack it).
- Modify: spec `2026-10-20-schema-user-supervisor-design.md` "Left to the builder": record delegation landed in PR C and the backoff numbers.

- [ ] **Step 1:** Docs edits; `make test` and `make test-all` — PASS (paste the tail).
- [ ] **Step 2: vmtest** (schema-vmtest skill, autologin desktop image) — each item is a check with expected output:
  - Boot → desktop up; `cat /sys/fs/cgroup/user.slice/user-1000.slice/user@1000.service/init.scope/cgroup.procs` = pid whose exe is `/usr/bin/schema-user`; `stat -c %U` of `user@1000.service` = the user; `schema-ctl --user status` (as the user) answers.
  - `kill -HUP` schema-logind → same `schema-user` pid after, log shows adoption, no second instance.
  - `kill -9` the `schema-user` → back within ~1 s with a new pid.
  - Crash loop: bind-mount a `#!/bin/sh\nexit 1` over `/usr/bin/schema-user`, kill it → log shows 1/2/4/8 s gaps then "giving up"; desktop untouched throughout.
  - Kill switch: boot with `schema.user=0` → no `user@1000.service/init.scope` process; desktop up.
  - `schema-ctl reboot` with a `.svc` in `$XDG_RUNTIME_DIR/schema-user/services` whose `ExecStop`-equivalent logs: the shutdown log shows `container/user-supervisor SIGTERM sent`, `schema-user-1000.log` shows the ordered stop before `SIGKILL broadcast`.
  - `loginctl enable-linger <user>` as the user → `/var/lib/systemd/linger/<user>` exists, `loginctl show-user <user> -p Linger` = yes; reboot → `schema-user` running before autologin; disable-linger → file gone.
- [ ] **Step 3: /code-review** (one pass, high). Fix what survives the shred.
- [ ] **Step 4:** Push branch `schema-user-lifecycle`, open **draft** PR, `gh pr checks` incl. CodeQL green.
- [ ] **Step 5: blakbox:** local RPM build from the branch (as PR B), `/root/svc-bak-<date>-prePRC`, `dnf install` → live re-exec of PID 1 (0 failed); `kill -HUP` schema-logind (never `restart`); expect `schema-user` running for 1000 in `user@1000.service/init.scope` with zero services. Then a reboot (Jonathan's go) and he confirms desktop + Deskflow/Obsidian/KDE Connect.
- [ ] **Step 6: Optiplex precondition** (spec §10): until Optiplex's session runs on `schema-dbus-session-run.sh` with the schema-dbus broker, it gets `/etc/schema-init/user-supervisor.disabled` **before** the dev COPR build carrying this lands there. Eli: no switch (already on schema-dbus).
