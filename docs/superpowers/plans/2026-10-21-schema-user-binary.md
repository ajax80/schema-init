# schema-user binary (PR B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `schema-user` binary that runs the shared engine (#395) as an unprivileged per-user supervisor, plus `schema-ctl --user` to drive it — run by hand only; nothing in the login path starts it yet.

**Architecture:** Finish the engine seam PR A left: every PID-1 path, fallback, log tag and wake source moves into `engine_paths`/`engine_hooks`/service globals, with PID 1's values as the defaults so PID 1 behaviour is unchanged. `schema-user.c` is a new `main()` over the same engine objects that fills those in from `$XDG_RUNTIME_DIR`/`$XDG_STATE_HOME`. A new `.svc` key `wait_socket=` holds a service until an AF_UNIX socket accepts (the session-bus gate).

**Tech Stack:** C99, the existing engine objects, POSIX sh integration test.

**Spec:** `docs/superpowers/specs/2026-10-20-schema-user-supervisor-design.md` — §2 and "Carried into PR B". §3 (cgroup delegation) and §4 (logind lifecycle) are the next PR (PR C); §5–§6 (`schema-import --user`, skip list) PR D; §7's shim mapping, §8, §9 PR E. This PR only *consumes* a delegated cgroup if one is already there.

## Global Constraints

- PID 1 behaviour byte-for-byte equivalent in effect: same boot, same ctl protocol, same re-exec blob compatibility. `main()` in `init.c` keeps `getpid() != 1` refusal.
- Runtime state under `$XDG_RUNTIME_DIR/schema-user/`: `ctl.sock`, `notify`, `services/`. Shm name `/schema-user-<uid>`. Timer stamps `$XDG_STATE_HOME/schema-user/timers` (default `~/.local/state/schema-user/timers`).
- No PID-1 work ever in `schema-user`: no mounts, watchdog, modprobe, reboot, console, PSI posture.
- `prctl(PR_SET_CHILD_SUBREAPER, 1)` at start.
- Service environment: `HOME`, `USER`, `LOGNAME`, `SHELL`, `PATH`, `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus`, plus environment.d replay over `/usr/lib/environment.d`, `/etc/environment.d`, `$XDG_CONFIG_HOME/environment.d` (that order, later wins, `*.conf` sorted). No `WAYLAND_DISPLAY`/`DISPLAY`.
- SIGTERM → ordered stop (same `engine_stop_services`/`engine_kill_services` as PID 1), then `_exit(0)`.
- Desktop login never depends on `schema-user`: this PR touches no logind, session script, or `.svc` on the fleet.
- `make test` stays green; `-Wall -Wextra` clean on new code. Why-comments only.

## Review Focus

1. **Second instance for the same uid** (hand run while one is up; later, a logind race) → the second refuses with "already running" and exits 1 before touching `ctl.sock` or starting anything. Test in Task 3.
2. **`XDG_RUNTIME_DIR` unset or not an existing dir owned by us** → exit 1 with a message naming it; never fall back to `/run` or cwd. Test in Task 3.
3. **Run as root** → refuse, exit 1 (root has PID 1; a root `schema-user` would write system cgroups and logs). Test in Task 3.
4. **Stale bus socket**: `$XDG_RUNTIME_DIR/bus` exists but nothing listens (bus died) → the gated service keeps waiting, it is not spawned. Test in Task 4.
5. **PID 1 defaults drift**: a new `engine_paths` field whose default isn't the old literal silently changes PID 1. `test_engine_conf.c` asserts every default. Test in Task 1.

---

### Task 1: Engine seams — paths, fallbacks, tag, logs, cgroup width (PID 1 unchanged)

**Files:**
- Modify: `engine.h` (`engine_paths_t`), `engine_conf.c` (defaults), `engine.c` (`timer_stamp_write` :122, `load_env_file` :320, `ctl_init` :1775–1795, `ctl_poll` :1974, reload `./services` :2131/:2142, `engine_load` :2291–2309, `engine_release` :2650, every `"[schema-init] "` printf), `init.c` (hardening resolve moves here), `service.h` (`cgroup_path[256]`, new globals), `service.c` (`service_cgroup_root` users :216–245, `svc_child_log` :1205, rail.log :1640)
- Test: `tests/test_engine_conf.c`

**Interfaces:**
- Produces in `engine_paths_t` (new fields; PID 1 default in parentheses): `ctl_fallback` (`"./run/schema-init.sock"`), `env_fallback` (`"./run/schema-init/env"`), `svc_fallback` (`"./services"`), `tag` (`"schema-init"`). `NULL` fallback = no fallback.
- Produces in `service.h`: `extern const char *service_log_dir;` (NULL = today's `/var/log/schema-init` → `/run/log/schema-init` chain); `service_cgroup_root` may be `""` = no cgroups (skip `cgroup_assign` entirely, `cgroup_path[0]=0`).
- Produces: `int engine_load(const char *svc_dir)` no longer reads `/proc/cmdline` or `hardening-default`; caller calls `service_set_hardening_default()` first. New `void pid1_hardening_default(void)` static in `init.c` holding the moved code, called before `engine_load` in `main()` and before the reexec-fallback `engine_load` at `init.c:809`.
- Produces: `int engine_mkdir_p(const char *path, mode_t mode)` in `engine.c`, exported in `engine.h`.

- [ ] **Step 1: Extend `tests/test_engine_conf.c`** with `test_pid1_defaults()` asserting `strcmp` equality for every `engine_paths` field against the literals above plus the existing ones (`CTL_SOCK_PATH`, `"/run/schema-init"`, `NOTIFY_SOCK_PATH`, `SCHEMA_SHM_NAME`, `TIMER_STAMP_DIR`, `SVC_DIR`), `service_cgroup_root` == `"/sys/fs/cgroup/schema-init"`, `service_log_dir == NULL`, `sizeof(((service_t*)0)->cgroup_path) == 256`; and `test_mkdir_p()`: `engine_mkdir_p("<mkdtemp>/a/b/c", 0700)` returns 0, the leaf is a dir, a second call returns 0. `engine_mkdir_p` lives in `engine.c`, so add `engine.c` to this test's link line in `Makefile`.
- [ ] **Step 2: Run** `make test` — expected: compile FAIL on the missing fields/function.
- [ ] **Step 3: Implement.** `timer_stamp_write` → `engine_mkdir_p(engine_paths.timer_stamp_dir, 0755)` (drop the `/var/lib` mkdirs). `ctl_init`: fallback block only when `engine_paths.ctl_fallback`; the `schema`-group chown/0660 only when `geteuid() == 0`, else `chmod 0600`. `ctl_poll`: refuse mutating verbs when `peer_uid != 0 && peer_uid != geteuid()` (PID 1: identical). `engine_release` unlinks `ctl_fallback` if set. `load_env_file`, reload and `engine_load` use their fallback fields, skipped when NULL. Log tag: replace `"[schema-init] ` in `engine.c` with a `"[%s] "` + `engine_paths.tag` argument (add a local `elog(fmt, ...)` printf wrapper if it reads better). `svc_child_log`/rail.log: when `service_log_dir` set, `engine`-style mkdir_p it and open `<dir>/<name>.log` / `<dir>/rail.log`, no `/var`/`/run` chain.
- [ ] **Step 4: Run** `make test` — expected: all PASS incl. `check_tu_statics: ok`; `make schema-init` warning count unchanged (only `init.c:133`).
- [ ] **Step 5: Commit** `engine: paths, fallbacks, log tag and log dir as engine settings`

### Task 2: Wake sources as hooks — no 250 ms idle tick without PSI

**Files:**
- Modify: `engine.h` (`engine_hooks_t`), `engine_conf.c`, `engine.c` (`get_poll_timeout` :2001–2062, loop :2580–2583), `init.c` (`pid1_loop_pressure` :1516, hook wiring :1683)
- Test: `tests/test_engine_conf.c`

**Interfaces:**
- Produces: `engine_hooks.loop_pressure` becomes `void (*)(int psi_fired)`; `loop_psi_fired` global removed.
- Produces: `int64_t (*wake_due)(int64_t now_m, int *tick)` in `engine_hooks_t`; default `engine_hook_wake_none` returns `INT64_MAX` (the engine's `WAKE_NONE`; use whatever constant `get_poll_timeout` uses) and leaves `*tick` alone. PID 1's `pid1_wake_due` in `init.c` returns the min of `watchdog` (`now_m + watchdog_pet_ms` when `watchdog_fd >= 0`), `reexec_check_due` (when `reexec_check_pid > 0 && !reexec_check_killed`), `cpu_check_due` (when any `PRIO_CRITICAL` service has a pid — the `critical` scan moves into the hook; it reads `services[]`), and sets `*tick = 1` when `psi_fd < 0`.
- `get_poll_timeout` keeps only engine wake sources (services, evictions, ready backstop/event) plus `engine_hooks.wake_due`.

- [ ] **Step 1:** In `test_engine_conf.c`, assert `engine_hooks.wake_due` is non-NULL and the default returns `INT64_MAX` with `tick` unchanged.
- [ ] **Step 2: Run** `make test` — FAIL (no member).
- [ ] **Step 3: Implement** as above; `engine_run` passes `psi_fired` to `engine_hooks.loop_pressure(psi_fired)`.
- [ ] **Step 4: Run** `make test` — PASS; `tests/test_next_wake.c` still PASS.
- [ ] **Step 5: Commit** `engine: PID 1 wake sources move behind a hook`

### Task 3: `schema-user` binary and `schema-ctl --user`

**Files:**
- Create: `schema-user.c`, `tests/test_schema_user.sh`
- Modify: `Makefile` (`ENGINE_OBJS = engine.o engine_conf.o schema.o service.o group.o caps.o ns.o landlock.o`; `schema-init: init.o $(ENGINE_OBJS)`; `schema-user: schema-user.o $(ENGINE_OBJS)` dynamic, `-lrt`; add to `BINS`, `install`, `test`), `schema-init.spec` (`%files` gets `%{_bindir}/schema-user`), `schema-ctl.c` (`--user`)

**Interfaces:**
- Consumes: Task 1 fields, Task 2 hooks, `engine_load/open/arm_timers/run/stop_services/kill_services/release/free`.
- Produces: `schema-user [-h|-V]` — no other args. Exit 1 with one stderr line for: `geteuid()==0` ("schema-user runs as a user; PID 1 supervises root"), `XDG_RUNTIME_DIR` unset / not a dir / not owned by euid, already running (connect to `ctl.sock` succeeds → "already running (pid from status)").
- Produces: `schema-ctl --user <cmd…>`: first argv; socket `$XDG_RUNTIME_DIR/schema-user/ctl.sock`, `SCHEMA_SVC_DIR` default `$XDG_RUNTIME_DIR/schema-user/services` for local verbs; error text names the user socket. No `--user` → unchanged.

Startup order in `main()` (all in `schema-user.c`):
1. guards above; `prctl(PR_SET_CHILD_SUBREAPER, 1)`; `umask(022)`.
2. `engine_paths`: `run_dir=$XDG_RUNTIME_DIR/schema-user`, `ctl_sock=<run_dir>/ctl.sock`, `notify_sock=<run_dir>/notify`, `svc_dir=<run_dir>/services`, `shm_name=/schema-user-<uid>`, `timer_stamp_dir=$XDG_STATE_HOME/schema-user/timers` (`$HOME/.local/state` default), all fallbacks NULL, `tag="schema-user"`. `service_log_dir=$XDG_STATE_HOME/schema-user/log`. `engine_mkdir_p` run_dir and svc_dir 0700.
3. Environment: from `getpwuid(geteuid())` set `HOME USER LOGNAME SHELL`; `PATH=/usr/local/bin:/usr/bin:/bin` unless set; `DBUS_SESSION_BUS_ADDRESS`; then environment.d replay — `static void envd_replay(void)`: for each dir, `scandir` `*.conf` alphasort, lines `KEY=VALUE`, skip blank/`#`, expand `$VAR`/`${VAR}` against the current environ, `setenv`. No shell eval.
4. Cgroups — `static void user_cgroup_root(void)`: read `0::<path>` from `/proc/self/cgroup`; root=`/sys/fs/cgroup<path>`. If `access(root/cgroup.procs, W_OK)==0` and `access(root, W_OK)==0` (delegated): mkdir `root/init.scope`, write own pid to its `cgroup.procs`, write `+<each in root/cgroup.controllers>` to `root/cgroup.subtree_control`, `service_cgroup_root=root`. Else `service_cgroup_root=""` and print one line `[schema-user] no delegated cgroup: running without per-service cgroups`. `root` buffer sized so `<root>/<name>` fits 256 — if `strlen(root) > 256-1-MAX_NAME`, treat as not delegated with that reason.
5. `service_set_hardening_default(0)`; signals: SIGTERM/SIGINT handler → `engine_running = 0`; ignore SIGPIPE. Hooks left default except `shut_log` → printf with tag.
6. `engine_load(svc_dir)` (cycle → print and exit 1), `engine_open(0)`, `engine_arm_timers(0)`, `engine_run()`, `engine_stop_services()`, `engine_kill_services()`, `engine_release()`, `_exit(0)`.

`tests/test_schema_user.sh` (skips with exit 0 if run as root): `T=$(mktemp -d)`; `XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state/nested XDG_CONFIG_HOME=$T/cfg`; `mkdir -m700 $T/run`; writes `.svc` files into `$T/run/schema-user/services/` in the repo's existing `.svc` syntax (copy key names from `services/*.svc`): `a` (sleeper writing a `TERM` trap timestamp to `$T/a.stop`), `b` depends on `a` (same, `$T/b.stop`), `flap` (exits 1 after 0.2 s, restart on), `dbl` (double-forks a `sleep 30` that writes `$PPID` to `$T/dbl.ppid`), `envdump` oneshot (`env > $T/env`), `tmr` persistent calendar timer (`echo`, daily 03:00); `$T/cfg/environment.d/50-t.conf` with `SCHEMA_T=hi-${HOME}`. Starts `./schema-user &`, then asserts within 5 s:
- `./schema-ctl --user status` lists `a`,`b` FUNDAMENTAL and `flap` with `restarts>=1`; `stat -c %a $T/run/schema-user/ctl.sock` is `600`
- `$T/env` contains `HOME=`, `XDG_RUNTIME_DIR=$T/run`, `DBUS_SESSION_BUS_ADDRESS=unix:path=$T/run/bus`, `SCHEMA_T=hi-$HOME`, no `WAYLAND_DISPLAY`
- `$T/dbl.ppid` equals the `schema-user` pid (subreaper)
- `$T/state/nested/schema-user/timers/tmr.stamp` exists; `$T/state/nested/schema-user/log/a.log` exists
- a second `./schema-user` exits 1 with "already running"; `env -u XDG_RUNTIME_DIR ./schema-user` exits 1; `XDG_RUNTIME_DIR=/nonexistent ./schema-user` exits 1
- idle: over 3 s, `voluntary_ctxt_switches` in `/proc/<pid>/status` grows by < 15 (a 250 ms tick would be ≥ 12 + restarts — kill `flap` first with `schema-ctl --user stop flap`)
- `kill -TERM`; process exits 0 within 5 s; `$T/b.stop` ≤ `$T/a.stop` (reverse dependency order); no leftover `sleep 30` from `dbl`.

- [ ] **Step 1:** Write `tests/test_schema_user.sh` as above; add `sh tests/test_schema_user.sh` to `make test` (with `schema-user schema-ctl` as prerequisites).
- [ ] **Step 2: Run** `make test` — FAIL (no `schema-user` target).
- [ ] **Step 3: Implement** `schema-user.c`, Makefile, spec `%files`, `schema-ctl --user`.
- [ ] **Step 4: Run** `make test` — PASS; `make schema-init && ls -l schema-init` still static (`file schema-init` says statically linked).
- [ ] **Step 5: Commit** `schema-user: per-user supervisor on the shared engine; schema-ctl --user`

### Task 4: `wait_socket=` — hold a service until a socket accepts

**Files:**
- Modify: `service.h` (`char wait_socket[108]; uint8_t sock_waiting;`), `service.c` (parse `wait_socket=` beside `condition` :1916, reject a non-absolute path like other path keys), `engine.c` (`STATE_NEW_PROCESS` after `service_deps_ready` :891; `get_poll_timeout`; status line :1507 / json :1462), `docs/` key reference (wherever `ready_path` is documented)
- Test: `tests/test_schema_user.sh`

**Interfaces:**
- No re-exec blob change: `wait_socket` is config (re-read from the `.svc`), `sock_waiting` is recomputed on the first pass after adopt.
- Produces: `static int socket_accepts(const char *path)` in `engine.c`: non-blocking `AF_UNIX SOCK_STREAM` connect, 1 = connected (close it), 0 otherwise.
- In `STATE_NEW_PROCESS`, after deps ready: `if (svc->wait_socket[0] && !socket_accepts(svc->wait_socket)) { if (!svc->sock_waiting) { svc->sock_waiting = 1; service_log(svc, "wait-socket"); } break; }` then `sock_waiting = 0`.
- `get_poll_timeout`: a service with `sock_waiting` is not a tick reason; it contributes `now_m + 1000` (it is in NEW_PROCESS, so exempt it from the `s != FUNDAMENTAL…` tick branch).
- Status: human line appends `  waiting=<wait_socket>` when `sock_waiting`; json adds `service.<name>.waiting=<path>`.

- [ ] **Step 1:** Add to the test: service `busy` with `wait_socket=$T/run/bus`. Assert after 2 s: status shows `waiting=$T/run/bus`, no `busy` process. Then create a stale socket (`python3 -c` bind + close without listen, leaving the file) — still waiting after 2 s. Then start a listener (`python3 -c` bind/listen/sleep 30 &) — within 3 s `busy` is FUNDAMENTAL with a pid.
- [ ] **Step 2: Run** `make test` — FAIL (unknown key / never waits).
- [ ] **Step 3: Implement** as above.
- [ ] **Step 4: Run** `make test` — PASS.
- [ ] **Step 5: Commit** `engine: wait_socket= holds a service until its socket accepts`

### Task 5: VM, re-exec and fleet verification; docs

**Files:**
- Modify: `docs/operations.md` (a `schema-user` section: what it is, Phase 1 status "run by hand; logind starts it in PR C", paths, `schema-ctl --user`, `wait_socket=`), `README.md` (one line under the binaries list)

- [ ] **Step 1:** `make test` rc=0 on the branch head.
- [ ] **Step 2:** schema-vmtest skill: boot the branch `schema-init` as PID 1 — 0 failed, ctl `status` output format identical to master's (diff a `status` capture from master's VM boot, names/states only).
- [ ] **Step 3:** In the VM: REEXEC master→branch and branch→master (the #393/#394 procedure) — both adopt, 0 failed, no service restarts.
- [ ] **Step 4:** In the VM as root, make a delegated tree by hand: `mkdir -p /sys/fs/cgroup/user.slice/user-1000.slice/user@1000.service`, chown it + `cgroup.procs cgroup.subtree_control cgroup.threads` to 1000, move a shell into it, run `schema-user` as uid 1000 from that shell with the Task 3 services. Expect: per-service dirs under `user@1000.service/`, `init.scope` holds `schema-user`, a `notify=1` service reaches ready. Then `schema-ctl reboot`-style shutdown: record whether PID 1's shutdown terminates it cleanly or hard-kills it — **evidence for PR C §3, not fixed here**.
- [ ] **Step 5:** blakbox: build the RPM from the branch onto blakbox only via the dev COPR path used for 1.1264, live re-exec, 0 failed, RSS normal. Then as ajax80 with a scratch `XDG_RUNTIME_DIR` (never the real `/run/user/1000/schema-user` — nothing on the desktop should see it): run `tests/test_schema_user.sh` against the installed `/usr/bin/schema-user`. Reboot blakbox once; desktop + autostart unchanged.
- [ ] **Step 6:** Write the docs; commit `docs: schema-user (phase 1, hand-run) and wait_socket=`.
- [ ] **Step 7:** Open the PR as **draft**; one `/code-review` pass; then ready.
