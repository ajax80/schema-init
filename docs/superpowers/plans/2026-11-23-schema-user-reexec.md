# schema-user re-exec Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `schema-ctl --user reexec` swaps a running `schema-user` onto a new binary in place, keeping every service, and `dnf upgrade` does it on its own.

**Architecture:** Move PID 1's re-exec machinery (`init.c` ~754–1290 plus its main-loop and `main()` glue) into `engine.c` behind host hooks, with PID 1's behaviour unchanged. `schema-user` then supplies its own hooks (lock fd, saved environment, cgroup root, last resort `_exit(1)`). `%posttrans` re-execs each live `schema-user` still running a deleted binary.

**Tech Stack:** C (static, gcc), POSIX sh tests, Python spec checks, RPM spec.

**Spec:** `docs/superpowers/specs/2026-11-23-schema-user-reexec-design.md`

## Global Constraints

- PID 1's `schema-ctl reexec` / `dnf upgrade` behaviour must not change: same replies, same refusals (pressure, verify gate, dry run, recheck), same fallback to the old image and reboot as last resort.
- Blob format: no field removed or renamed; new key `lock=` only (old readers skip unknown keys). A v0.4.32 PID 1 blob must still be adopted.
- argv on every exec stays `{ path, "--reexec", <blob>, <oldexe>, NULL }` / `{ path, "--reexec-check", <blob>, NULL }`, except argv[0] for schema-user is the argv[0] it was started with.
- schema-user execs (dry run, commit, fallback) pass the deep-copied startup environment; PID 1 passes `environ`.
- schema-user re-exec skips `run_import()`.
- Empty service set refuses only when `!engine_reload_allow_empty`.
- Code style: no docstrings; why-comments only.

## Review Focus

1. A user with zero services runs `reexec` → it succeeds (`0 adopted, 0 new`), not "no services loaded". Test in Task 2.
2. A running service's pid and cgroup are unchanged after `reexec`, and it is still supervised (killing it gets it restarted by the new image). Test in Task 2.
3. A new binary that fails the dry run (e.g. a `#!/bin/sh` script that prints `err:` and exits 1) → old image keeps running, reply starts `err:`, service untouched, `schema-ctl --user status` still answers. Test in Task 2.
4. After `reexec`, SIGTERM still stops schema-user cleanly (held signals unblocked) and a second `reexec` works (lock fd still held: a second hand-started `schema-user` still says "already running"). Test in Task 2.
5. `.svc` edited between dry run and commit into a cycle → refused, old image keeps running. Covered by the shared recheck already; Task 2 adds a cycle-at-dry-run case (cheaper to provoke).

---

### Task 1: Move re-exec into engine.c (PID 1 unchanged) + `lock=` blob key

**Files:**
- Modify: `engine.c`, `engine.h` (new hooks + `engine_reexec_*` API)
- Modify: `init.c` (delete moved code; register PID 1 hooks; `main()` calls the engine API)
- Modify: `reexec_state.h` (`int fd_lock` in `reexec_global_t`, written as `lock=`, parsed, default `-1`)
- Test: `tests/test_reexec_state.c`

**Interfaces:**
- Produces, in `engine.h`:
  - `engine_hooks_t` gains:
    - `int (*reexec_refuse)(char *err, size_t errsz);` — non-zero refuses with `err` (PID 1: pressure)
    - `void (*reexec_save_host)(reexec_global_t *g);` — PID 1: watchdog fd, booted_version/image, init_start, nofile, stall/reclaim, argv0
    - `void (*reexec_restore_host)(const reexec_global_t *g);` — PID 1: watchdog (CLOEXEC, pet-ms, pet), the same fields back
    - `void (*reexec_check_prep)(void);` — before validate in the dry run (PID 1: `pid1_hardening_default()`)
    - `void (*reexec_last_resort)(void);` — no old image to return to (PID 1: sync + reboot)
  - `extern char **engine_reexec_env;` — environment for all re-exec `fexecve`s; NULL means `environ`
  - `extern const char *engine_reexec_argv0;` — argv[0] for the commit/fallback exec; NULL means the path (PID 1 fallback keeps `/sbin/schema-init`)
  - `extern int engine_reexec_lock_fd;` — host fd carried as `lock=`, kept through stray-close, CLOEXEC cleared at commit; `-1` for PID 1
  - `int engine_reexec_start(int client, const char *path, char *err, size_t errsz);`
  - `int engine_reexec_check_main(int blob_fd);` — the `--reexec-check` body, returns exit code
  - `void engine_reexec_adopt(int blob_fd, int oldexe_fd);`
  - `void engine_reexec_finish(void);` — after `engine_arm_timers()`: unblock held signals, send the pending reply
  - main-loop integration (`reexec_check_deadline`, reap of the dry run, wake deadline) moves into the engine's own loop/reap paths, not host hooks.

- [ ] **Step 1: Write the failing test** — in `tests/test_reexec_state.c`, add `test_lock_fd_roundtrip`: a `reexec_global_t` with `fd_lock = 7` written by `state_write` and read by `state_parse` gives `fd_lock == 7`; a blob without `lock=` (use an existing v-current fixture string) parses with `fd_lock == -1`.
- [ ] **Step 2: Run** `make test` → FAIL (no `fd_lock` member).
- [ ] **Step 3: Implement** `fd_lock` in `reexec_state.h` beside `fd_oldexe` on the same `fd` record.
- [ ] **Step 4: Run** the reexec-state test → PASS.
- [ ] **Step 5: Move the code.** Cut `init.c`'s re-exec block into `engine.c` under the names above, replacing PID-1-only lines with the hooks / globals. `reexec_validate()`'s empty check becomes `svc_count < 0 || (svc_count == 0 && !engine_reload_allow_empty)`. `held_signals()` moves with it. `init.c` registers its hooks before the `--reexec-check` / `--reexec` branches and keeps rail/board handling keyed off `reexec_fd >= 0`.
- [ ] **Step 6: Verify PID 1 is unchanged** — `make clean && make && make test` → all PASS; then run the `schema-vmtest` skill and in the guest `schema-ctl reexec` → `ok: re-executed into …, N services adopted, 0 new`, and `schema-ctl reexec /bin/false` → `err:` reply, PID 1 still answers `schema-ctl status`.
- [ ] **Step 7: Commit** `engine: move re-exec out of init.c behind host hooks`.

### Task 2: schema-user re-exec

**Files:**
- Modify: `schema-user.c`
- Modify: `schema-ctl.c` (usage line for `--user reexec`; gate stays PID-1-only as today)
- Create: `tests/test_schema_user_reexec.sh`; Modify: `Makefile` (`test:` runs it after `test_schema_user.sh`)

**Interfaces:**
- Consumes: everything Task 1 produces.
- Produces: `reexec [<path>]` on the user ctl socket (default path = the startup argv[0]); reply lines identical in shape to PID 1's.

- [ ] **Step 1: Write the failing test** `tests/test_schema_user_reexec.sh`, harness copied from the head of `tests/test_schema_user.sh` (temp XDG dirs, `SCHEMA_USER_IMPORT=`, `until_ok`, `fail`), copy `./schema-user` to `$T/su-old` and `$T/su-new` and start `$T/su-old`. Cases:
  - **zero services:** empty services dir, `schema-ctl --user reexec $T/su-new` → reply matches `^ok: re-executed into .*, 0 services adopted, 0 new`.
  - **adopt:** restart with one sleeper `a`; record `pid=` from status and `/proc/<pid>/cgroup`; `reexec $T/su-new` → `ok:`; same pid, same cgroup line; `readlink /proc/$PID/exe` is `$T/su-new`; `/proc/$PID/cmdline` argv[0] is still `$T/su-old`; `kill <a pid>` → `a` comes back with a new pid.
  - **refused dry run:** `$T/bad` = `#!/bin/sh` printing `err: nope` and exit 1; `reexec $T/bad` → reply starts `err:`; same schema-user pid, exe still `$T/su-new`, `a` pid unchanged, status answers.
  - **cycle at dry run:** add `x.svc dep=y`, `y.svc dep=x`, `reexec $T/su-new` → `err:` containing `cycle`; remove both.
  - **lock + signals:** second `reexec` → `ok:`; starting another `$T/su-new` exits non-zero with `already running`; `kill -TERM $PID` → exits 0 within 10 s and `a`'s `.stop` stamp exists.
  - **environment:** an `envdump` oneshot added after the re-exec (`schema-ctl --user add`) still sees `XDG_RUNTIME_DIR=$T/run`.
- [ ] **Step 2: Run** `sh tests/test_schema_user_reexec.sh` → FAIL (`reexec` unknown on the user socket).
- [ ] **Step 3: Implement in `schema-user.c`:**
  - `static char **saved_env;` deep copy of `environ` at the top of `main()`, before anything reads or changes it; `engine_reexec_env = saved_env`; `engine_reexec_argv0 = argv[0]`.
  - Argument parse: accept `--reexec <blob> <oldexe>` and `--reexec-check <blob>` (replacing `if (argc > 1) die(...)` for those forms only). Path setup (`run_dir`, `svc_dir`, `engine_paths.*`, `service_set_hardening_default(0)` via `reexec_check_prep`) runs first; `--reexec-check` returns `engine_reexec_check_main()` before `single_instance()`, mkdirs, cgroup or import.
  - `single_instance()` stores its fd in `static int lock_fd`; on `--reexec` it is not called — the `lock=` fd from the blob is taken (via `reexec_restore_host`) and re-`flock`ed `LOCK_EX|LOCK_NB` (same open file, succeeds).
  - `reexec_save_host`: `g->fd_lock = lock_fd`. `engine_reexec_lock_fd = lock_fd`.
  - `user_cgroup_root()`: strip a trailing `/init.scope` from `cg_root` before the delegation checks (fresh start never has it, so a no-op there).
  - `reexec_last_resort`: `_exit(1)`.
  - `--reexec` path: `engine_reexec_adopt()` in place of `run_import()` + `engine_load()`; then `engine_open(1)`, `engine_arm_timers(1)`, `engine_reexec_finish()`, `engine_run()` as normal.
  - `user_ctl_extra`: `reexec` / `reexec <path>` → `engine_reexec_start(fd, path, …)`; on `-1` reply `err: <err>`.
- [ ] **Step 4: Run** `sh tests/test_schema_user_reexec.sh` → PASS; `make test` → all PASS.
- [ ] **Step 5: Commit** `schema-user: re-exec in place, keeping its services`.

### Task 3: Upgrade trigger + docs

**Files:**
- Modify: `schema-init.spec` (`%posttrans`, after the PID 1 re-exec block, outside it; `%changelog` entry under the unreleased section)
- Modify: `docs/roadmap.md` (tick the item), `README.md` schema-user section (one line on `schema-ctl --user reexec`)
- Create: `tests/test_spec_user_reexec.py`; Modify: `Makefile` `test:` runs it

**Interfaces:**
- Consumes: Task 2's `schema-ctl --user reexec`.

- [ ] **Step 1: Write the failing test** `tests/test_spec_user_reexec.py` in the style of `tests/test_spec_user_import.py`. Checks on the `%posttrans` body: contains `pgrep -x schema-user`; contains `*" (deleted)")`; contains `cmp -s /proc/$pid/exe %{_bindir}/schema-user && continue`; contains `XDG_RUNTIME_DIR="/run/user/$uid" %{_bindir}/schema-ctl --user reexec`; contains `log out and back in to finish`; the block appears after `schema-ctl reexec ||` and not inside the `reexec-pending` `if`; the extracted block passes `sh -n` (replace `%{_bindir}` with `/usr/bin` first). Roadmap line starts `- [x] \`schema-user\` re-adopts`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** the block (`stat -c %u /proc/$pid` for uid, skip a uid already done, skip when `/run/user/$uid/schema-user/ctl.sock` is not a socket, never fail the transaction), message `schema-init: schema-user for uid $uid kept its old binary; log out and back in to finish`. Changelog + roadmap + README lines.
- [ ] **Step 4: Run** the test → PASS; `make test` → PASS.
- [ ] **Step 5: Commit** `rpm: re-exec running schema-user instances on upgrade`.

### Task 4: Land and verify (no code)

- [ ] Push, open **draft** PR, run one `/code-review`, fix, CI green, merge.
- [ ] Dev COPR build lands; on blakbox `sudo dnf --refresh upgrade schema-init` → PID 1 re-exec `ok:` (same adopted count as before), each schema-user re-exec `ok:` (first upgrade from a schema-user built before this merge prints the "log out and back in" line — then log out/in once, upgrade again with the next dev build or `schema-ctl --user reexec` by hand).
- [ ] Rootless podman check on blakbox: a running container's conmon pid unchanged across `schema-ctl --user reexec`.
- [ ] Eli: dev upgrade + reboot → `schema-ctl verify` clean, schema-user up, `schema-ctl --user reexec` → `ok:`.
