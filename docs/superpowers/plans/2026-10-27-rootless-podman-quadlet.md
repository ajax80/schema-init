# Rootless podman + Quadlet on schema-user — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Quadlet `.container` in the user's dirs runs as a supervised schema-user service that starts at boot (lingering), restarts on crash, has kernel-enforced and verified limits, publishes LAN ports, and leaves nothing behind on stop.

**Architecture:** `schema-import --user` runs Quadlet's generator and imports its units. The shared engine (`engine.c`/`service.c`, used by PID 1 and schema-user) gains four opt-in `.svc` keys: `exec_stop=`/`exec_stop_post=`, `main_pid_notify=1`, `delegate=1`, `verify_*_max=`. No existing `.svc` changes behaviour.

**Tech Stack:** C (engine, schema-user), Python 3 (schema-import), sh test harnesses, QEMU vmtest.

**Spec:** `docs/superpowers/specs/2026-10-27-rootless-podman-quadlet-design.md` (approved 2026-10-27). Read it first; this plan does not repeat its reasoning.

## Global Constraints

- New keys are opt-in; a `.svc` without them must behave byte-for-byte as today (spec anti-goal).
- A limit that can't be applied or verified fails the start loudly; never run unlimited quietly.
- PID 1 never blocks: every new child (stop commands) is spawned and reaped from the main loop, bounded by `stop_timeout_sec`.
- New runtime fields go in `SVC_RUNTIME_FIELDS` (`reexec_state.h`) so re-exec and reload keep them.
- `service.c` has two parse chains ("keep the two parse chains in sync"); every new key goes in both.
- Each new key gets a row in `docs/svc-reference.md`.
- Engine changes: `make test` green + a vmtest boot before any hardware reboot.
- schema-user onto a new binary: SIGKILL it (logind restarts it), never SIGTERM. schema-logind: `kill -HUP`.
- Each task = one draft PR off master, one `/code-review` pass, merge before the next PR that consumes it.
- Fedora 44 x86_64; rootless ports ≥ 1024; `net.ipv4.ip_unprivileged_port_start` untouched.

## Review Focus

1. A broken `.container` (Quadlet exits non-zero or prints an error) must not abort the import: other units still import, the error is logged. → Task 1 test `generator_failure_tolerated`.
2. Deleting a `.container` then `daemon-reload`: the service is evicted, its `exec_stop` runs, `podman ps -a` is empty. → Task 1 test `deleted_container_drops_unit` + Task 6 step.
3. Limits written as `--memory=64m`, `-m 1g`, `--cpus=1.5`, `--pids-limit 100` (equals and space forms, short flag, unit suffixes) all map. → Task 5 test `verify_keys_forms`.
4. A container that exits on its own: `Restart=always` → respawn; no `Restart=` → stays down (EXCISED), no hot loop. → Task 3 vmtest cases `adopted_exit_restart` / `adopted_exit_norestart`.
5. A hand-written unit of the same name in `~/.config/systemd/user` wins over the generated one. → Task 1 test `config_shadows_generated`.

---

### Task 1: Importer — Quadlet generator, enabled links, specifiers

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-import.py` (`_unit_dirs` ~1053, `user_enabled_set` ~1813, `_specifiers` ~146, env translation, `run_user_import` ~2152, `_user_translate` ~1965)
- Test: `tests/test_import_user.py`

**Interfaces:**
- Produces: `run_user_generators(ctx: dict, log=print) -> str` — returns the generator root `$XDG_RUNTIME_DIR/schema-user/generator`; creates/empties `normal/ early/ late/`; runs each executable in `GENERATOR_DIRS = ("/usr/lib/systemd/user-generators", "/usr/local/lib/systemd/user-generators", "/etc/systemd/user-generators")` (under `_root()`), argv `[gen, normal, early, late]`, 5 s timeout each; nonzero exit/timeout → `log("WARN    generator %s: ...")`, continue.
- `_unit_dirs()` in user mode inserts `early` first, `normal` after `$XDG_RUNTIME_DIR/systemd/user`, `late` last (systemd order: early > config > … > normal > … > late).
- `user_enabled_set` reads `.wants`/`.requires` from `[cfg, normal]` instead of `[cfg]`.

- [ ] **Step 1: Write failing tests** in `tests/test_import_user.py` using the `Tree` fixture and a fake generator script installed under `t.root/usr/lib/systemd/user-generators/` that writes `$1/foo.service` (`[Service] ExecStart=/bin/foo %n` + `Environment=U=%n`) and links `$1/default.target.wants/foo.service`:
  - `generated_unit_imported`: `foo.svc` exists, `args=foo.service`, `env=U=foo.service` (no literal `%n`).
  - `generated_wants_enabled`: `foo.service` in `user_enabled_set` units.
  - `generator_args`: the fake generator records `$#` = 3 and three distinct dirs.
  - `generator_failure_tolerated`: a second generator `exit 1` → `foo.svc` still generated, a `WARN    generator` line logged.
  - `deleted_container_drops_unit`: second `run_user_import` after the fake generator stops writing `foo.service` → no `foo.svc`.
  - `config_shadows_generated`: `~/.config/systemd/user/foo.service` with `ExecStart=/bin/mine` → `exec=/bin/mine`.
  - `restart_carried`: the fake generator's unit has `Restart=always` → `.svc` has no `no_restart=1`.
  - `podman_wait_mapped`: generated unit with `Wants=/After=podman-user-wait-network-online.service` → `dep=network-online`, and no `podman-user-wait-network-online.svc` written.
- [ ] **Step 2: Run** `python3 tests/test_import_user.py` → the 8 new checks FAIL.
- [ ] **Step 3: Implement** `run_user_generators`, call it at the top of `run_user_import`, `dry_run` included (it writes only under its own generator dir), extend `_unit_dirs`/`user_enabled_set`; apply `_specifiers` to `Environment=` values too (today only argv); treat `podman-user-wait-network-online.service` as the existing `NET_ONLINE` wait (add it to the alias path `_after_online` checks, and to the never-imported set).
- [ ] **Step 4: Run** `python3 tests/test_import_user.py && python3 tests/test_import_units.py` → PASS; `make test` rc=0.
- [ ] **Step 5: Real Quadlet check** on blakbox: `~/.config/containers/systemd/probe.container` (spec's minimal file) → `SCHEMA_SVC_DIR=$(mktemp -d) python3 …/schema-import.py --user --dry-run` shows `probe` generated with `notify=1` and expanded `%n`. Remove the file after.
- [ ] **Step 6: Commit + draft PR** `schema-import --user: run user generators (Quadlet)`.

### Task 2: `exec_stop=` / `exec_stop_post=`

**Files:**
- Modify: `service.h` (fields), `service.c` (both parse chains; a spawn helper), `engine.c` (`ctl` stop/restart ~1666–1705, `reap` ~610, eviction ~234–330, `shutdown_ordered` ~2477), `reexec_state.h` (runtime fields), `schema-import.py` (map `ExecStop=`/`ExecStopPost=`), `docs/svc-reference.md`
- Test: `tests/test_svc_stopcmds.c` (new, parse), `tests/test_schema_user.sh` (behaviour), `tests/test_import_units.py`, `Makefile` (wire the new C test like `test_svc_rlimits`)

**Interfaces:**
- Produces in `service_t`: `char *exec_stop[MAX_EXEC_PRE]; int exec_stop_n; char *exec_stop_post[MAX_EXEC_PRE]; int exec_stop_post_n;` — same storage/argv form and leading-`-` ignore-failure rule as `exec_pre` (`svc_run_pre`, service.c:760).
- Runtime (in `SVC_RUNTIME_FIELDS`): `pid_t stop_pid; int stop_phase; /* 0 none, 1 exec_stop, 2 exec_stop_post */ int stop_idx; int64_t stop_due_ms;`
- `void service_stop_cmd_spawn(service_t *svc, int phase, int idx)` in service.c: forks, applies the service's user/groups (no namespaces, no landlock), puts the child in the service cgroup for phase 1 and leaves it in the supervisor's cgroup for phase 2, execs. Never waits.
- Engine: `static void stop_begin(service_t *svc)` — if `exec_stop_n`, run `exec_stop` commands in order (each awaited via `reap`, all bounded together by `stop_timeout_sec` or 3 s), *then* SIGTERM the main pid; else SIGTERM at once (today's path). After the service is fully gone (any death, crash included), run `exec_stop_post` the same way. A non-`-` command failing is logged (`stop-cmd-failed`), never blocks the stop.

- [ ] **Step 1: Failing parse test** `tests/test_svc_stopcmds.c`: `exec_stop=/usr/bin/podman rm -f x` ×2 + `exec_stop_post=-/bin/false` → `exec_stop_n == 2`, argv split like `exec_pre`, ignore flag set on the post entry; 9 lines → load refused like `exec_pre`'s 8 limit.
- [ ] **Step 2: Failing behaviour cases** in `tests/test_schema_user.sh`: service `sc` with `exec_stop=/bin/sh -c "date +%s%N > $T/sc.stopcmd"` and the sleeper's TERM stamp:
  - `schema-ctl --user stop sc` → `sc.stopcmd` exists and its time < `sc.stop` (command before SIGTERM).
  - `exec_stop_post` stamp exists after stop, and after `kill -9` of the main pid (crash path).
  - hung `exec_stop=/bin/sleep 100`, `stop_timeout_sec=2`: `schema-ctl --user status` answers within 1 s during the stop, and the service is down within 4 s.
- [ ] **Step 3: Run** `make test` → new checks FAIL.
- [ ] **Step 4: Implement** fields, both parse chains, `service_stop_cmd_spawn`, `stop_begin`; route ctl `stop`, `restart`, eviction kills, and `shutdown_ordered` through `stop_begin`; `reap` matches `stop_pid` before service pids and advances `stop_idx`/phase; a tick checks `stop_due_ms` and SIGKILLs an overdue stop command. Importer: `ExecStop=`/`ExecStopPost=` → `exec_stop=`/`exec_stop_post=` keeping a leading `-`, specifiers expanded, max 8 each (warn + drop extras).
- [ ] **Step 5: Run** `make test` → PASS. Importer test: Quadlet unit from the spec → `exec_stop=/usr/bin/podman` argv with `systemd-probe`, `exec_stop_post=-/usr/bin/podman …`.
- [ ] **Step 6: vmtest** (`/schema-vmtest`): boot rail clean, shutdown clean with one `exec_stop` service in the image.
- [ ] **Step 7: Docs + commit + draft PR** `engine: exec_stop= / exec_stop_post=`.

### Task 3: `main_pid_notify=1` — follow `MAINPID=`

**Files:**
- Modify: `notify.h` (`struct notify_msg` + `notify_parse`), `service.h`/`service.c` (key, both chains; load refuses `main_pid_notify=1` without `notify=1`), `engine.c` (notify handler ~1950, `reap` ~610, stop paths), `reexec_state.h`, `schema-import.py`, `docs/svc-reference.md`
- Test: `tests/test_notify.c`, `tests/livetest/mainpid-vmtest.sh` (new, modelled on `notifycap-vmtest.sh`, reusing `notifycap/notifier.c`), `tests/test_import_units.py`

**Interfaces:**
- `struct notify_msg` gains `pid_t mainpid;` (0 = absent; parse `MAINPID=<1..INT_MAX>`, garbage ignored).
- `service_t`: flag `SVC_MAIN_PID_NOTIFY`; runtime `pid_t main_pid; int main_wait; /* launcher exited 0, waiting for MAINPID */ int64_t main_wait_due_ms;` in `SVC_RUNTIME_FIELDS`.
- Acceptance check: `static int mainpid_ok(const service_t *svc, pid_t p)` — `kill(p,0)==0`, `svc->cgroup_path[0]`, and `notify_read_cgroup(p,…)` + `notify_cgroup_text_matches(…, svc->cgroup_path)` (descendants allowed). Not `pid_file_read()`.
- Wait bound: `MAINPID_WAIT_SECS 10` (engine.c, beside `PID_FILE_WAIT_SECS`).

State rules (spec §2, verbatim intent):
- `MAINPID=` while launcher runs → store in `main_pid`; `child_pid` untouched.
- Launcher exits non-zero (any time before adoption) → `service_cgroup_kill`, recovery arc (`died`).
- Launcher exits 0, `main_pid` valid → `child_pid = main_pid`, log `main-pid pid=N`, keep state.
- Launcher exits 0, no `main_pid` → `main_wait=1`, due = now+10 s; `MAINPID=` arriving then adopts; due passes → kill cgroup, recovery arc, log `main-pid-timeout`.
- After adoption the adopted pid is reaped as `child_pid` (schema-user is subreaper; PID 1 is init), so existing death/stop/shutdown paths apply unchanged.

- [ ] **Step 1: Failing parse tests** in `tests/test_notify.c`: `"MAINPID=1234\nREADY=1"` → `mainpid==1234 && ready`; `"MAINPID=abc"`, `"MAINPID=0"`, `"MAINPID=-5"` → `mainpid==0`.
- [ ] **Step 2: Failing vmtest** `tests/livetest/mainpid-vmtest.sh <schema-init> <schema-ctl> <serial-out>`; services (busybox + `notifier`), each prints a `MAINPID-CASE <name> PASS|FAIL` console line checked by the script:
  - `before_exit`: launcher forks `sleep 1000`, sends `MAINPID=<child>` + `READY=1`, exits 0 → status shows the sleeper's pid, state FUNDAMENTAL.
  - `after_exit`: launcher exits 0, child sends `MAINPID` 2 s later → adopted.
  - `launcher_fails`: launcher sends `MAINPID` then exits 1 → `died`, cgroup gone.
  - `never`: launcher exits 0, nothing sent → `main-pid-timeout` at ~10 s.
  - `outside`: `MAINPID=1` → ignored, then timeout.
  - `adopted_exit_restart` / `adopted_exit_norestart`: adopted child exits → respawn / EXCISED (Review Focus 4).
  - `stop`: `schema-ctl stop` SIGTERMs the adopted pid.
  - `reexec`: `schema-ctl reexec` with an adopted service → same `main_pid` after.
  - `no_key`: same launcher without `main_pid_notify=1` → behaves as today (launcher exit 0 = service ended).
- [ ] **Step 3: Run** both → FAIL.
- [ ] **Step 4: Implement**; importer: `Type=notify` + `NotifyAccess=all` → `main_pid_notify=1`.
- [ ] **Step 5: Run** `make test` + `mainpid-vmtest.sh` → all PASS; full `/schema-vmtest` boot PASS.
- [ ] **Step 6: Docs + commit + draft PR** `engine: main_pid_notify=1 follows MAINPID=`.

### Task 4: `delegate=1` — limits can apply

**Files:**
- Modify: `service.h`, `service.c` (`cgroup_assign` ~226, `service_cgroup_kill` ~1631 / `cgroup_kill_path`, both parse chains), `schema-import.py` (`Delegate=yes` → `delegate=1`), `docs/svc-reference.md`
- Test: `tests/livetest/delegate-vmtest.sh` (new), `tests/test_import_units.py`

**Interfaces:**
- Flag `SVC_DELEGATE`. With it, `cgroup_assign` makes `<svc>/` with `+cpu +memory +cpuset +pids +io` in its `cgroup.subtree_control`, places the spawned pid in leaf `<svc>/<LEAF>`, and under PID 1 with `user=` chowns `<svc>/` plus its `cgroup.procs`, `cgroup.subtree_control`, `cgroup.threads` to that user. Any step failing → the child does not exec (`HARDENING FAILED for <name>: delegate`), recovery arc — never runs undelegated.
- `service_cgroup_kill` removes the tree depth-first after `cgroup.kill`.
- `<LEAF>` and the final layout are decided in Step 1.

- [ ] **Step 1: Determine podman's split layout.** In a vmtest image or on blakbox inside schema-user's delegated tree, start `podman run --cgroups=split --memory 64m -d busybox sleep 1000` from a process sitting in a delegated leaf; record the resulting cgroup tree (where conmon and the payload land, names used). Pick `<LEAF>` so podman's split works and test 4 can pass; write the observed tree into the PR description. Done when the tree is recorded and `<LEAF>` chosen.
- [ ] **Step 2: Failing vmtest** `delegate-vmtest.sh`: a `delegate=1` service that `mkdir`s a child cgroup, writes `memory.max=67108864` into it, moves a `sleep` there, prints the read-back; checks: value read back equals 67108864; service cgroup's `cgroup.procs` empty; `schema-ctl stop` leaves no cgroup dir behind; a `user=nobody` + `delegate=1` service can do the same; a `delegate=1` service whose cgroup root can't enable controllers (simulate by pre-creating `<svc>` with a process in it) → `HARDENING FAILED … delegate` and not running.
- [ ] **Step 3: Run** → FAIL.
- [ ] **Step 4: Implement**; importer mapping + test (`Delegate=yes` → `delegate=1`; `KillMode=mixed` needs no key — ctl stop already SIGTERMs only the main pid, then the cgroup is killed; note it in a why-comment).
- [ ] **Step 5: Run** `make test` + vmtest → PASS.
- [ ] **Step 6: Docs + commit + draft PR** `engine: delegate=1`.

### Task 5: `verify_*_max=` — check the limits landed

**Files:**
- Modify: `schema-import.py` (podman argv → keys), `service.h`/`service.c` (keys, both chains), `engine.c` (check after READY, in the notify handler), `docs/svc-reference.md`
- Test: `tests/test_import_units.py`, `tests/test_svc_verify.c` (new: pure compare + tree walk over a fake dir), `Makefile`

**Interfaces:**
- Keys (sysfs form, strings): `verify_memory_max=<bytes>`, `verify_cpu_max=<quota> <period>`, `verify_pids_max=<n>`.
- Importer: `podman_limit_lines(argv: list[str]) -> list[str]` — only when `argv[0]` basename is `podman` and `run` is in argv. Forms: `--memory X`, `--memory=X`, `-m X`; `--cpus X|=X`; `--pids-limit X|=X`. Memory suffix b/k/m/g (×1024ⁿ, case-insensitive). CPUs: `"%d 100000" % round(float(X)*100000)`. Unparseable → `WARN … not verified`, no key.
- C: `int verify_limits_match(const char *cg_dir, const service_t *svc, char *why, size_t n)` (service.c, pure over a directory path) — walks descendants of `cg_dir`, skipping `<LEAF>` from Task 4; passes when some populated descendant (`cgroup.events` `populated 1`) has every set `verify_*` file equal to the expected string; else fills `why` with `memory.max want=… got=…`.
- Engine: on accepted `READY=1` for a service with any `verify_*`, call it; mismatch → log `limits-mismatch <why>`, `service_cgroup_kill`, recovery arc.

- [ ] **Step 1: Failing importer test** `verify_keys_forms` (Review Focus 3): `--memory=64m` → `verify_memory_max=67108864`; `-m 1g` → `1073741824`; `--cpus=1.5` → `verify_cpu_max=150000 100000`; `--pids-limit 100` → `verify_pids_max=100`; `--memory 64x` → no key + warning; non-podman exec → no keys.
- [ ] **Step 2: Failing C test** `tests/test_svc_verify.c`: temp tree `svc/LEAF`, `svc/payload/{memory.max,cgroup.events}` → match passes on equal, fails with `want=67108864 got=max` on `max`, fails when the only matching dir is unpopulated.
- [ ] **Step 3: Run** → FAIL.
- [ ] **Step 4: Implement.**
- [ ] **Step 5: Run** `make test` → PASS; vmtest boot PASS.
- [ ] **Step 6: Docs + commit + draft PR** `engine: verify_*_max= checks container limits after READY`.

### Task 6: End-to-end on blakbox

**Files:**
- Create: `tests/livetest/podman-quadlet-e2e.sh` (runs the spec's success test against a live box, prints PASS/FAIL per step)
- Modify: `docs/roadmap.md` (tick the podman line), `docs/svc-reference.md` (a short "Quadlet / rootless podman" section)

**Interfaces:** consumes all keys above via the importer; no new code paths.

- [ ] **Step 1: Install** the merged dev build on blakbox (`dnf upgrade 'schema-init*'` from COPR dev only; PID 1 re-execs), swap schema-user with SIGKILL. Confirm versions with `rpm -q`.
- [ ] **Step 2: Test container** `~/.config/containers/systemd/e2e-web.container`: `Image=docker.io/library/nginx:alpine` (my call; anything serving :80), `PublishPort=8080:80`, `Memory=64m`, `[Service] Restart=always`, `[Install] WantedBy=default.target`. `loginctl enable-linger ajax80` (or schema-logind's equivalent) confirmed.
- [ ] **Step 3: Script** `podman-quadlet-e2e.sh` checks spec tests 2–7 (login, `kill -9` conmon's payload → back within 30 s, `podman exec` memory hog OOM-killed with `memory.max`=67108864, `curl http://<blakbox-ip>:8080` from GreenPi over ssh, stop → `podman ps -a` empty, edit `Memory=128m` + `daemon-reload` + `restart` → 134217728), plus the deleted-file case (Review Focus 2) and a cold run with the image removed first.
- [ ] **Step 4: Reboot** blakbox (Jonathan's go-ahead first) → spec test 1: container up before login (`podman ps` via ssh before the desktop session starts). Then run the script → all PASS.
- [ ] **Step 5: Clean up** the test container, image, and linger if it was off before.
- [ ] **Step 6: Docs + commit + draft PR** `docs: rootless podman + Quadlet`; tick roadmap.
