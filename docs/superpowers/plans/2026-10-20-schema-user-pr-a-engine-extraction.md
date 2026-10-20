# schema-user PR A — engine extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move schema-init's supervisor engine out of `init.c` into `engine.c`/`engine.h` so a second binary (`schema-user`, PR B) can link it — with **no change in PID 1 behaviour**.

**Architecture:** Three mechanical steps, each its own commit and test cycle: (1) every hard-coded path the engine uses becomes a field of one `engine_paths` struct whose defaults are today's constants; (2) every PID-1-only call made from inside engine code goes through one `engine_hooks` struct that `init.c` fills; (3) the engine functions and state move verbatim to `engine.c`, `init.c` keeps PID-1-only code and `main()`. Then VM + hardware verification.

**Tech Stack:** C99, GNU make, existing `tests/test_*.c` style (self-contained asserting programs run by `make test`), schema-vmtest skill (QEMU/KVM boot of PID 1).

**Spec:** `docs/superpowers/specs/2026-10-20-schema-user-supervisor-design.md` §1 ("Two programs, one engine").

**Later plans (written after this PR lands and is reboot-verified on blakbox):** PR B `schema-user` binary + per-user cgroup delegation; PR C schema-logind lifecycle (start/stop/grace/adopt/linger/kill switch); PR D `schema-import --user` + packaging + skip list; PR E `schema-ctl --user` + systemctl shim; PR F session D-Bus activation helper + Python shim `StartUnit` forwarding; then the per-host rollout (Optiplex schema-dbus precondition first).

## Global Constraints

- PID 1 behaviour must be equivalent in effect: same boot sequence, same ctl protocol and replies, same shm layout (`SCHEMA_SHM_MAGIC 0x53434d03u` unchanged), same cgroup paths (`/sys/fs/cgroup/schema-init/<name>`), same timer stamp dir (`/var/lib/schema-init/timers`), same sockets (`/run/schema-init.sock`, `/run/schema-init/notify`).
- **Re-exec blob compatibility both directions**: `reexec_state.h` structs and blob format are not touched. An old PID 1 must re-exec into the new binary and the new into the old.
- `main()` keeps the `getpid() != 1` refusal (`init.c:3884`).
- No new comments/docstrings beyond the surrounding density; no behaviour "improvements" ride along. A function moves byte-for-byte except: `static` dropped where `init.c` now calls it, path constants → `engine_paths.*`, PID-1-only calls → `engine_hooks.*`.
- `schema-init-static` and the `aarch64`/`armhf` targets must still build.
- Session-holding daemons are never restarted live during verification (schema-logind via `kill -HUP`, schema-dbus at reboot).

## Review Focus

1. **Re-exec across the boundary** (old binary → new, new → old) — a moved global that the blob writer/reader touches through a different symbol would silently lose state. Pinned by Task 4 Step 3.
2. **ctl `reboot`/`poweroff`/`reexec` commands** — they set PID-1-only state from inside `ctl_cmd`; after the split they must still reboot/poweroff/re-exec. Pinned by Task 4 Step 2 (vmtest `schema-ctl poweroff` + `reboot`).
3. **Shutdown ordering** — `shutdown_ordered()` moves to the engine but `shut_log` stays PID 1's; a hook that's NULL at shutdown time would crash PID 1 at poweroff. Pinned by Task 2 Step 1 (`engine_hooks` must never be NULL-called; test asserts defaults are no-ops) and Task 4 Step 2.
4. **Timer stamps / Persistent timers** after the path change — a wrong `timer_stamp_dir` makes every `Persistent=` timer fire at boot. Pinned by Task 1 Step 1 and Task 4 Step 2 (check `/var/lib/schema-init/timers` stamps unchanged across boot).
5. **Static build** — moving to two translation units can expose a symbol clash with `schema.c`/`service.c` names only in `-static` link. Pinned by Task 3 Step 4.

---

### Task 1: Engine paths struct

**Files:**
- Create: `engine.h`
- Modify: `init.c` (every use of `CTL_SOCK_PATH`, `NOTIFY_SOCK_PATH`, `"/run/schema-init"` mkdir, `SCHEMA_SHM_NAME`, `TIMER_STAMP_DIR`, `svc_dir` default), `service.c:221-229,336,357,1549` (cgroup root literal)
- Test: `tests/test_engine_conf.c`, `Makefile` (`test:` target)

**Interfaces:**
- Produces (in `engine.h`):
  ```c
  typedef struct {
      const char *ctl_sock;         /* default CTL_SOCK_PATH "/run/schema-init.sock" */
      const char *run_dir;          /* default "/run/schema-init" */
      const char *notify_sock;      /* default NOTIFY_SOCK_PATH "/run/schema-init/notify" */
      const char *shm_name;         /* default SCHEMA_SHM_NAME "/schema-init" */
      const char *timer_stamp_dir;  /* default "/var/lib/schema-init/timers" */
      const char *svc_dir;          /* default SVC_DIR */
  } engine_paths_t;
  extern engine_paths_t engine_paths;
  ```
  and in `service.h`: `extern const char *service_cgroup_root;` (default `"/sys/fs/cgroup/schema-init"`, defined in `service.c`).
- `CTL_SOCK_PATH` moves from `init.c:44` to `engine.h` (schema-ctl keeps its own copy; unchanged). `TIMER_STAMP_DIR` moves to `engine.h`.

- [ ] **Step 1: Write the failing test** `tests/test_engine_conf.c` — links `engine_conf.c service.c schema.c group.c caps.c ns.c landlock.c` like `test_service_env.c` and includes `engine.h`. `engine_conf.c` holds the single `engine_paths` definition, linked by `schema-init` now and `schema-user` later. Assertions:
  ```c
  assert(!strcmp(engine_paths.ctl_sock, "/run/schema-init.sock"));
  assert(!strcmp(engine_paths.run_dir, "/run/schema-init"));
  assert(!strcmp(engine_paths.notify_sock, "/run/schema-init/notify"));
  assert(!strcmp(engine_paths.shm_name, "/schema-init"));
  assert(!strcmp(engine_paths.timer_stamp_dir, "/var/lib/schema-init/timers"));
  assert(!strcmp(service_cgroup_root, "/sys/fs/cgroup/schema-init"));
  /* cgroup path follows the root */
  service_cgroup_root = "/tmp/cg-test";
  service_t s = {0}; strcpy(s.name, "foo");
  assert(!strcmp(service_cgroup_path_for(&s), "/tmp/cg-test/foo"));
  ```
  Add `const char *service_cgroup_path_for(const service_t *svc)` (returns a static buffer, used by `cgroup_assign`) so the path rule is testable without touching `/sys`.
- [ ] **Step 2: Run** `make test` — expect compile failure (`engine.h` missing).
- [ ] **Step 3: Implement.** Create `engine.h`, `engine_conf.c` (the one `engine_paths_t engine_paths = {...}` initialiser); add `service_cgroup_root` + `service_cgroup_path_for` in `service.c`; replace each literal/define use listed under **Files** with the field. Add `engine_conf.c` to `SRCS` and to the test link line. `svc_dir`'s default in `main()` reads `engine_paths.svc_dir`.
- [ ] **Step 4: Run** `make test` → all pass; `make schema-init schema-init-static` → builds, `-Wall -Wextra` clean (no new warnings vs `git stash`-ed baseline: compare `make 2>&1 | grep -c warning`).
- [ ] **Step 5: Confirm no literal left:** `grep -n '"/run/schema-init\|"/sys/fs/cgroup/schema-init\|SCHEMA_SHM_NAME\|TIMER_STAMP_DIR\|NOTIFY_SOCK_PATH' init.c service.c` → only the `engine_conf.c`/`engine.h`/`service.c` default definitions and `cpuset.cpus.exclusive` paths built from `service_cgroup_root`.
- [ ] **Step 6: Commit** `engine: paths struct, no behaviour change`.

### Task 2: Engine hooks struct

**Files:**
- Modify: `engine.h`, `engine_conf.c` (hooks definitions beside the paths), `init.c`
- Test: `tests/test_engine_conf.c`

**Interfaces:**
- Produces (in `engine.h`):
  ```c
  typedef struct {
      void (*loop_pressure)(void);            /* PSI + survival posture + reclaim block, init.c main loop */
      void (*loop_tail)(void);                /* eviction_tick(); watchdog_pet(); reexec_check_deadline(); */
      int  (*ctl_extra)(int fd, char *line);  /* reexec / reboot / poweroff; return 1 if handled */
      void (*shut_log)(const char *msg);
      void (*on_sigcont)(void);               /* runit_stop_request */
  } engine_hooks_t;
  extern engine_hooks_t engine_hooks;
  void engine_hook_noop(void);
  int  engine_hook_ctl_none(int fd, char *line);   /* returns 0 */
  void engine_hook_log_none(const char *msg);
  ```
  Defaults in `engine_conf.c` are the no-op functions above — never NULL.
- `service_watchdog_tick()` stays engine (per-service watchdog, not the hardware one). The hardware `watchdog_pet()` is PID-1-only → `loop_tail`.

- [ ] **Step 1: Add to the test** (`tests/test_engine_conf.c`): assert every `engine_hooks` field is non-NULL at startup and that calling each default returns without effect (`engine_hooks.ctl_extra(-1, buf) == 0`).
- [ ] **Step 2: Run** `make test` → fails (no `engine_hooks`).
- [ ] **Step 3: Implement.** In `init.c`, wrap the main-loop pressure block (`init.c` main loop: the `{ int mem = psi_fired || … reclaim_maybe(); }` block) as `static void pid1_loop_pressure(void)` (it needs `psi_fired` — make that a file-static set by the poll section), the three tail calls as `pid1_loop_tail`, the `reexec`/`reboot`/`poweroff` branches of `ctl_cmd` as `pid1_ctl_extra` (called first from `ctl_cmd`; if it returns 1, `ctl_cmd` returns), SIGCONT's `runit_stop_request` as `on_sigcont`. `main()` assigns `engine_hooks` to the `pid1_*` functions before `setup_signals()`. `shutdown_ordered()` and `shut_reap()` call `engine_hooks.shut_log` instead of `shut_log`.
- [ ] **Step 4: Run** `make test` → pass; build clean.
- [ ] **Step 5: Commit** `engine: hooks struct for PID-1-only calls, no behaviour change`.

### Task 3: Move the engine to `engine.c`

**Files:**
- Create: `engine.c`
- Modify: `init.c`, `engine.h`, `Makefile` (`SRCS`, static target, cross targets if they list sources)

**Interfaces:**
- Consumes: Task 1/2 `engine_paths`, `engine_hooks`.
- Produces (`engine.h`) — the API PR B's `schema-user` main calls, and `init.c`'s `main()` now calls:
  ```c
  int  engine_load(const char *svc_dir);     /* load_tables(); returns cycle count like load_tables */
  void engine_open(int adopting);            /* shm_init, mkdir run_dir, ctl_init + notify_open unless adopting, inotify, signalfd_init */
  void engine_arm_timers(int adopting);      /* the timer-arming block after schema_boot_log */
  void engine_run(void);                     /* the while (running) loop */
  void engine_stop_services(void);           /* listen_close all + shutdown_ordered + service_cgroup_kill all */
  void engine_release(void);                 /* close/unlink notify + ctl, munmap/shm_unlink, free argv/landlock */
  extern volatile int engine_running;        /* was `running` */
  extern volatile int engine_do_reboot;      /* was `do_reboot` */
  ```
  plus `extern` declarations for the engine state `init.c`'s remaining PID-1 code reads (services/shadow tables, `svc_count`, groups, `shm_ptr`, `notify_fd`, `ctl_fd`, `sig_fd`, reexec-touched fields) — exactly the set the compiler demands, nothing more.
- **Stays in `init.c`:** `main`, `usage`, modprobe/`pci_hex`/`sp5100_present`/watchdog module + `watchdog_*`, `mount_pseudo`, `pstore_*`, `cleanup_tmp_locks`, `rescue_boot`, PSI/pressure/reclaim/`execute_survival_posture`/`set_cgroup_freeze`/`set_cgroup_cpu_limit`/`eviction_tick`, all `reexec_*`, `schema_boot_log`, `shut_console_open`/`shut_write`/`shut_flush_log`/`shut_log`, `fuse_abort_all`, `container_cgroups_*`, `bounded_sync`, `read_mount_line`/`root_source`/`remount_all_ro`, `sig_*` handlers that set reboot state, the `pid1_*` hooks.
- **Moves to `engine.c`:** timers (`timer_*`, `calendar_recent_occurrence`, `mono_sec`), `service_watchdog_tick`, `load_env_file`, `restart_budget_refresh`, `fallback_*`, `park`, `ctl_rearm`, `play_card`/`card_done`, `mark_boot_ready`, `note_exit`, `pid_file_read`, `fork_adopt`, `timer_rearm`, `notify_poll`/`notify_pause_track`, `reap`, `ready_*`, `tick_service`, `shm_init`/`shm_update`, `ctl_*` (incl. `ctl_cmd`, `ctl_analyze`, `an_*`), `set_nonblock`, `fdstore_keep`, `signalfd_init`, `get_poll_timeout`, `validate_and_resolve`, `handle_reload`, `load_tables`, `start_failsafe`/`monitor_failsafes`/`active_kill_service`/`execute_fuse_cmd`, `shut_depends_on`/`svc_signal_all`/`shut_reap`/`shutdown_ordered`.
- If a function on the "moves" list turns out to call PID-1-only code not covered by a Task 2 hook, stop and add a hook field (and its default + test assertion) rather than moving PID-1 code into the engine.

- [ ] **Step 1: Move.** Cut/paste function by function in source order; drop `static` only where `init.c` calls the function; rename `running`→`engine_running`, `do_reboot`→`engine_do_reboot`. Split `main()` into the `engine_*` calls in the same order as today (PID-1 setup → `engine_load` / rescue-on-cycles stays in `main` → `engine_open` → `reap()` on adopt → `engine_arm_timers` → re-exec reply → `engine_run` → watchdog_close/console/container SIGTERM → `engine_stop_services` → container kill / `kill(-1)` → `engine_release` → PID-1 sync/remount/reboot).
- [ ] **Step 2: Prove it's a move.** `git diff --color-moved=zebra --color-moved-ws=ignore-all-space HEAD -- init.c engine.c | grep -c '^\(+\|-\)' ` reviewed by eye: every non-moved changed line is one of: `static` removed, `running`/`do_reboot` rename, `engine_*` wrapper, `extern` decl. Paste the non-moved lines into the commit message body.
- [ ] **Step 3: Run** `make test` and `make test-all` → pass.
- [ ] **Step 4: Build all variants:** `make schema-init schema-init-static` and `make aarch64` (cross toolchain present on blakbox) → clean, no new warnings.
- [ ] **Step 5: Commit** `engine: move supervisor engine to engine.c, no behaviour change`.

### Task 4: Verify PID 1 equivalence (VM, then blakbox)

**Files:** none changed (test logs saved under `tests/livetest/` like prior PRs: `engine-split-*-20261020.txt`)

- [ ] **Step 1: Unit + suite:** `make test && make test-all` on the branch head → pass.
- [ ] **Step 2: VM boot (schema-vmtest skill).** Boot the new binary as PID 1. Pass criteria: all rail services reach their normal state (`schema-ctl status`, 0 failed), a timer with `Persistent=` does **not** fire at boot when its stamp is current (check stamp mtimes in `/var/lib/schema-init/timers` before/after), `schema-ctl reboot` reboots, then `schema-ctl poweroff` powers off cleanly with the ordered-stop log lines present (`stop 1 (+… ms):`, `services down in … ms`).
- [ ] **Step 3: Re-exec both directions in VM** (REEXEC=1 mode used for #394): old master `100e971` → branch binary via `schema-ctl reexec`, then branch → old master. Pass: PID stays 1, every service keeps its pid and state, 0 missed watchdog pets, notify/ctl keep working (`schema-ctl status` after each hop).
- [ ] **Step 4: blakbox.** Build RPM or install the binary as for #393/#394 (`dnf upgrade` path re-execs PID 1 live — that's Step 3's old→new on hardware), then **reboot with Jonathan's OK**. Pass: `rpm -q`/binary hash matches, 0 failed, Plasma session up, PID 1 RSS within ±200 kB of #150's 4204 kB, `schema-analyze record` + `regress` → no regressions.
- [ ] **Step 5: PR as draft**, body lists Steps 1–4 evidence; one `/code-review` pass; mark ready only after review. Bump spec version before any tag.
