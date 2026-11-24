# schema-user re-exec — design

Roadmap item: "`schema-user` re-adopts running services across its own restart,
so containers survive an upgrade." Jonathan picked approach A on 2026-11-23.

## Why

Today a package upgrade never touches a running `schema-user`: `%posttrans`
re-execs PID 1 only, so each `schema-user` keeps running its deleted old binary
until logout or reboot. The only way to put the new binary in place is to
restart it, and a restart kills the whole user tree: logind
`kill_user_cgroup()`s on any exit, on purpose, since the orphans would
otherwise run unsupervised and then twice. Rootless podman/Quadlet containers
under `schema-user` therefore die on every real `schema-user` update.

Goal: `dnf upgrade` puts every running `schema-user` on the new binary, and its
services and containers keep running, same pids, same cgroups.

## Goals

- `schema-ctl --user reexec [<path>]`: the running `schema-user` swaps its
  binary in place (execve, same pid), keeping every running service.
- Same safety as PID 1's re-exec: sealed-memfd state blob, dry run of the new
  binary as a child before commit, recheck at commit, fall back to the old
  image if the new one refuses the state.
- The RPM upgrade re-execs every running `schema-user` on its own.
- One re-exec implementation shared by PID 1 and `schema-user`.

## Non-goals

- Surviving a `schema-user` crash or a kill with a new pid (approach C). A crash
  still means logind kills the tree and starts fresh.
- logind watching for a deleted binary. The package scriptlet is the trigger.
- Changing PID 1's re-exec behaviour. Its path is moved, not redesigned.

## Anti-goals (a failure even if it "works")

- Any change in what PID 1 does on `schema-ctl reexec` / `dnf upgrade`.
- A refused or failed re-exec that leaves services unsupervised or doubled.
- A `schema-user` that comes back from re-exec with no `XDG_RUNTIME_DIR` / wrong
  paths, or that fails its own single-instance lock against itself.

## Approaches considered

- **A — move PID 1's re-exec into `engine.c` behind hooks; `schema-user` uses it
  (picked).** One copy; PID 1's code is already proven. Cost: touches PID 1's
  re-exec path, so it takes the full gate (vmtest, then a hardware reboot).
- B — copy the code into `schema-user.c`. PID 1 untouched, but ~400 duplicated
  lines that drift. Rejected for the drift.
- C — a fresh `schema-user` adopts orphans after a real restart. No `waitpid`
  for non-children; needs pidfd exit-status tricks (newer kernels only). Also
  covers crashes, but far more fragile. Rejected.

## Design

### Shared code (engine.c)

Move from `init.c` (lines ~754–1290 today) into `engine.c`: blob write/read,
validate, compare, dry-run start/deadline/done, recheck, commit, adopt, stray
fd close, fallback. The PID-1-only parts become hooks or host fields:

| PID 1 specific                                        | becomes |
|-------------------------------------------------------|---------|
| hardware watchdog fd (keep, pet before exec)          | host fd in the blob; `-1` for schema-user |
| pressure refusal (`system_under_pressure`)            | `reexec_refuse` hook (PID 1 only) |
| booted_version / booted_image / init_start / nofile / stall times | host save/restore hooks |
| `pid1_hardening_default()` in the dry run             | host hook (schema-user: `service_set_hardening_default(0)`) |
| fallback with no old image: `reboot()`                | hook: PID 1 reboots; schema-user `_exit(1)` |
| blocked signals across exec (kernel drops SIG_DFL to PID 1) | keep for both; harmless for schema-user |
| argv[0] / exec path (`/sbin/schema-init`)             | host-provided |
| rail / board after re-exec                            | stays in init.c, keyed off "was re-execed" |

The blob format (`reexec_state.h`) does not change shape; schema-user writes
zeros/`-1` for the PID 1 fields. A PID 1 blob written by the old image must
still be read by the new one (upgrade from v0.4.32 PID 1): no field removed or
renamed.

### schema-user specifics

- **Environment.** `service_environment()` replaces the environment at start,
  and the new image needs the original (`XDG_RUNTIME_DIR`, `XDG_STATE_HOME`,
  `XDG_CONFIG_HOME`, `SCHEMA_USER_IMPORT`, `HOME` …). Keep a copy of the startup
  environment before it is replaced and pass that copy to `fexecve`.
- **Instance lock.** The lock fd crosses the exec (CLOEXEC cleared at commit,
  its number in the blob or argv); the new image takes it over instead of
  opening the lock file again (a second open would conflict with itself).
- **No import on re-exec.** The new image skips `run_import()`; a changed
  importer would change the `.svc` set and make the compare refuse. The next
  `daemon-reload` or login picks up importer changes.
- **Command.** `reexec` is accepted on the user ctl socket from the user and
  from root (the existing peer rule). `schema-ctl --user reexec` skips
  `verify_gate` (that guards PID 1). Default path `/usr/bin/schema-user`.
- **Cgroup root.** `user_cgroup_root()` derives the root from
  `/proc/self/cgroup`, which after the first start reads `…/init.scope`. The
  root is carried in the blob (or `/init.scope` stripped); a re-exec'd image
  must create services beside `init.scope`, never under it.
- **argv[0].** The new image keeps the argv[0] the instance was started with
  (`/usr/bin/schema-user`), whatever `<path>` was re-exec'd: logind's
  `_is_schema_user()` matches on exe or argv[0]/argv[1], and a miss at its next
  adoption would make it kill the user's tree as leftovers.
- **Fallback.** New image refuses → back to the old image (same as PID 1). Old
  image gone too → `_exit(1)`; logind sees a crash, kills the tree, restarts.

### Traps found in review (Greg, 11-23, verified against the code)

- **Dry run vs the instance lock.** `--reexec-check` runs while the parent
  holds the flock; it must compute `run_dir`/`svc_dir` from the saved
  environment and return its verdict before `single_instance()`, the import,
  or any mkdir.
- **Empty service set.** `reexec_validate()` refuses `svc_count <= 0`; shared
  code refuses 0 only when `!engine_reload_allow_empty`. Otherwise every user
  with nothing enabled refuses re-exec in `%posttrans`.
- **Lock fd carried.** New blob key `lock=` (the parser skips unknown keys, so
  old images are unaffected; `fd_watchdog` is not overloaded). It goes in
  `keep[]` for `reexec_close_strays()` and has CLOEXEC cleared at commit.
- **Saved environment on all three execs.** Dry-run child, commit, and
  fallback all `fexecve` with the deep-copied startup environment, never the
  current `environ`. PID 1 passes `environ` as today.
- **Signals and reply after adopt.** PID 1 unblocks the held signals and sends
  the pending `schema-ctl` reply in `main()` after `engine_arm_timers()`. That
  moves into shared code run after adopt, so schema-user never keeps SIGTERM
  blocked and its client gets an answer.
- **Hooks (builder's naming may vary):** `reexec_refuse` (PID 1 pressure),
  `reexec_save_host` / `reexec_restore_host` (PID 1: watchdog + boot metrics;
  schema-user: lock fd, cgroup root), `reexec_check_prep` (hardening default),
  `reexec_last_resort` (PID 1 reboot; schema-user `_exit(1)`).
- **`%posttrans` discovery:** `pgrep -x schema-user`, `/proc/<pid>/exe` ends in
  ` (deleted)`, uid from `stat -c %u /proc/<pid>`, once per uid, socket must
  exist.

### Upgrade trigger

`%posttrans`, after the PID 1 block but not inside it or dependent on it (my
call): schema-user and PID 1 only meet through logind and the cgroup tree, so
a new schema-user under an old PID 1 is the same mix a user gets today between
upgrade and reboot. Only instances whose `/proc/<pid>/exe` is the replaced
binary (`… (deleted)`, as the schema-udev block checks) are re-exec'd (my call):
a hand-run dev instance on another binary, or one already current, is left
alone. For each, run `XDG_RUNTIME_DIR=/run/user/<uid> schema-ctl --user
reexec`. On error print one line per user per transaction naming the user and
"log out and back in to finish"; no state kept (my call). A `schema-user` from
v0.4.32 or older does not know `reexec` and gets that line.

## Testing

- Unit/C tests: blob round trip with schema-user host fields; adopt with the
  lock fd and environment carried.
- `tests/test_schema_user*`: hand-run schema-user with a long-running service,
  `reexec` → same service pid, same cgroup, `status` answers, new version in
  `--version`/log. Refused path: a new binary that fails the dry run → old
  image keeps running, service untouched.
- PID 1: existing re-exec tests stay green; vmtest PASS; hardware reboot +
  `dnf upgrade` re-exec on blakbox (68/0 as before).
- Live: blakbox rootless podman container under schema-user survives
  `schema-ctl --user reexec` and an RPM upgrade.

## Landing (my call)

Draft PR → /code-review → merge when green → dev COPR. Ships in v0.4.33 with
#578, no separate release. Hardware: blakbox takes the dev build by
`dnf upgrade` (PID 1 re-exec + schema-user re-exec, podman container check);
Eli takes a reboot on it as the hardware test box.

## Left to the builder

- Exact hook names and where the host-specific fields live.
