# PID 1 re-exec: swap the schema-init binary without a reboot

**Status:** Draft 2026-09-30, Greg review folded in (async dry run, stray-fd sweep, overlay→reap order). Awaiting Jonathan's answers to the open questions.
**Component:** `init.c` (`main`, `ctl_cmd`, `handle_reload`), new pure `reexec_state.h`
(+ `tests/test_reexec_state.c`), `schema-ctl.c` (one verb). RPM `%posttrans` is a later PR.

## Problem

Every PID 1 change costs a reboot of every host. Idle PID 1 (#219) on 2026-09-30 took
four reboots (blakbox, DBox, Eli, Optiplex); the umask fix (#204) took three on one night.
Rebooting is also why PID 1 fixes ship slowly: a reboot interrupts whatever the host is
doing (Optiplex was mid-stream when #219 rolled out). `dnf upgrade` already replaces
`/usr/bin/schema-init` on disk. The running PID 1 keeps the old, now `(deleted)`, image
until the next boot.

SIGHUP is config reload, and it stays that. Re-exec is a separate, explicit verb.

## Goal

`schema-ctl reexec` replaces the PID 1 image in place:

- Same PID 1, same children. No service is stopped, restarted or re-spawned. `restarts=`
  and all states are unchanged.
- Timers keep their deadlines. Completed run-once boot timers stay completed.
- The control socket and NOTIFY socket never close. A client that connects during the
  swap waits in the listen backlog. It is never refused.
- The hardware watchdog is never closed, so it can't fire and can't be disarmed.
- A signal sent during the swap (`reboot`, `poweroff`) is delivered to the new image,
  not dropped.
- A bad new binary is caught **before** PID 1 commits to it. If it still fails after
  the exec, the old image is executed again from a held fd.

## Design

### 1. Runtime state has one definition

`handle_reload` (`init.c:1676`) already copies the runtime half of `service_t` from
the live table into the freshly parsed shadow, by name. That list has had three
must-carry bugs (cgroup_path, is_frozen, run-once timer), each found in production.
Re-exec needs exactly the same list. Two hand-kept copies would drift.

One X-macro in `reexec_state.h` names every runtime field once, with its type:

```c
#define SVC_RUNTIME_FIELDS(X) \
    X(PID,  child_pid)        X(INST, inst)            X(INT, restart_count) \
    X(U8,   dormant_count)    X(TS,   dormant_until)   X(TIME, last_start) \
    X(TIME, start_time)       X(TS,   stable_time)     X(PID, failsafe_pid) \
    X(TS,   failsafe_start)   X(TS,   last_pet)        X(INT, ready_path_verified) \
    X(INT,  notify_ready)     X(STR,  notify_status)   X(INT, ctl_killed) \
    X(INT,  exit_status)      X(INT,  term_signal)     X(INT, core_dumped) \
    X(INT,  has_exited)       X(TS,   timer_next)      X(TS,  spawn_time_mono) \
    X(STR,  cgroup_path)      X(INT,  is_frozen)
```

It expands to `svc_runtime_copy(dst, src)`, which `handle_reload` calls in place of its
23 assignments, and to the serializer and parser below. The run-once timer fact
(`SVC_TIMER` cleared at runtime) rides as a derived `timer_done` key. It is applied
with the same rule `handle_reload` uses. Adding a runtime field later means one line.

### 2. State blob: versioned text in a sealed memfd

The old image writes the blob to `memfd_create("schema-init-state", MFD_ALLOW_SEALING)`,
seals it (`F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK`), and passes its fd number in argv.
It is text, not a struct dump, because `service_t` changes layout across exactly the
versions this feature exists to bridge.

```
schema-init-state 1
global version=0.4.0-1.972.git8bd6033 init_start=12.345678901 argv0=/sbin/schema-init
global under_pressure=0 last_stall_ms=0 last_reclaim_ms=0
fd ctl=3 notify=4 watchdog=5 client=6 oldexe=7
svc name=dbus hash=2779096485 child_pid=199 state=4 prev=3 weight=8 target=4 ...
svc name=boot-timing hash=... timer_done=1 ...
evict pid=4411 deadline=1759284000 cgroup=/sys/fs/cgroup/schema-init/foo
end
```

- One record per line, `key=value` tokens. Values are percent-escaped (space, `%`,
  `=`, control bytes). `notify_status` is daemon-supplied text, so it is never trusted
  raw.
- `end` is mandatory. A blob without it is truncated and rejected.
- **Compatibility:** the header carries an integer format version. A reader accepts any
  version ≤ its own, ignores unknown keys, and defaults missing ones. That lets an older
  image (the rollback target) read a newer blob. The version is bumped only when a field
  changes meaning, and an unknown higher version is rejected.
- `state_write()` / `state_parse()` are pure (FILE* in/out, no globals) and fully
  unit-tested: round-trip, escaping, truncation, unknown key, version gate,
  `MAX_SERVICES` overflow.

### 3. Old image: `schema-ctl reexec [PATH]`

The verb is root-only (not in `ctl_is_readonly`). PATH defaults to the `argv[0]` PID 1
booted with (`/sbin/schema-init`), never `/proc/self/exe`, because after an upgrade that
is the *old* inode. An explicit PATH is for hand-testing a build before it is packaged.

**Refuse up front** (reply `err:`, nothing changes):
- shutting down (`running == 0`)
- `system_under_pressure`. Swapping images while memory-starved is the wrong moment,
  so the reply says to retry.
- PATH not a regular executable file

**Dry run — the safety gate.** Write the blob, then fork. The child execs
`PATH --reexec-check <fd>`. That is a non-PID-1 mode `main` allows. It parses the blob,
loads `/etc/schema-init/services` exactly as the real path will, runs
`validate_and_resolve`, applies the checks below, prints one line, and exits 0 or 1.
**PID 1 does not wait on it.** It records `reexec_check_pid`, holds the client fd, and
goes back to the main loop: petting the watchdog, reaping, answering `status`. A 5 s
deadline joins `get_poll_timeout`. Blocking instead would starve the watchdog. The fleet's
shortest pet is 10 s, but a 1 s chip pets every 333 ms. `reap()` matches the check
child's pid. A deadline that passes first gets a SIGKILL and a refusal. While a check is
pending, a second `reexec` is refused, and shutdown cancels the check. The check child's
stdout is a pipe, read when it exits. Anything except exit 0 refuses the re-exec and
relays the child's line to the client.

The dry-run blob is a validation snapshot. Commit writes a **fresh** blob, because state
moves during the check (a service can die or respawn). Just before writing it, the old
image re-runs the integrity and orphan checks itself: they are cheap and need no new
code. A `.svc` edited during the 5 s window is still caught. The dry run catches:
- a binary that crashes or doesn't run
- an incompatible blob version
- a dependency cycle
- **the reload integrity rule:** a `.svc` whose `content_hash` differs from the blob's.
  Re-exec is "same config, new code", and a changed `.svc` still waits for boot (see
  open question 1).
- **orphans:** a service in the blob with a live child but no `.svc` on disk. The reply
  says to run `schema-ctl reload --evict` first, so the new image never loses track of a
  running process.

**Commit:**
1. `open("/proc/self/exe", O_RDONLY)` → `oldexe` fd (the rollback image, valid even if
   unlinked).
2. Block SIGTERM, SIGINT, SIGUSR1, SIGUSR2 (SIGCHLD and SIGHUP are already blocked).
   **This is required, not cosmetic:** execve resets handled signals to SIG_DFL, and the
   kernel drops SIG_DFL signals sent to PID 1. Blocked signals stay pending across
   execve, so a `reboot` sent during the swap is delivered once the new image installs
   its handlers.
3. Clear `FD_CLOEXEC` on ctl, notify, watchdog, the client connection, oldexe and the
   blob. Everything else closes on exec: signalfd, inotify and the PSI fd are
   re-created.
4. Pet the watchdog, `fflush(NULL)`, `execv(PATH, {PATH, "--reexec", "<fd>", NULL})`.
5. If execv returns: restore CLOEXEC, unblock the four signals, close oldexe and the blob,
   and reply `err: exec <PATH>: <errno>`. PID 1 continues exactly as before.

### 4. New image: `--reexec <fd>`

`main` parses `--reexec` before the PID-1 positional-arg skip. With it, boot changes:

| boot step | on re-exec |
|---|---|
| umask, `service_raise_pid1_nofile`, `coredump_take_pattern`, hardening default | run (idempotent) |
| TIOCNOTTY, `mount_pseudo`, module loading | **skip** (already done) |
| `cleanup_tmp_locks` | **skip — it would delete live X/Xwayland locks** |
| `watchdog_init` | **skip.** Adopt the fd, re-read the timeout via WDIOC_GETTIMEOUT, pet immediately |
| `ctl_init`, `notify_open` | **skip.** Adopt the fds, so the sockets never unlink or rebind |
| services/groups load + `validate_and_resolve` | run (same as boot; the dry run proved it) |
| state overlay | `svc_runtime_copy` by name from the blob; restore evictions, `init_start`, pressure globals |
| timer arming | only timers with `timer_next == 0` (newly added), the same rule as reload |
| `shm_init`, `psi_open`, inotify, `signalfd_init` | run (fresh fds; `ready_watched=0` re-arms watches on the next pass) |
| `setup_signals` | run, **then unblock** SIGTERM/INT/USR1/USR2. Pending ones fire now |
| `schema_boot_log` | replaced by `[schema-init] re-exec git8bd6033 → gitabc1234: 41 adopted, 0 new` |
| stray fds | close every fd that is not 0–2 and not named in the blob's `fd` line. Several transient `fopen`/`open` calls lack CLOEXEC; one caught mid-flight must not leak into the new image forever |
| ordering | overlay **→** `reap()` **→** `signalfd_init`. Exits during the swap are zombies, and only the restored `child_pid` can attribute them, so reaping before the overlay would lose them. SIGCHLD pending across exec is still reported by the new signalfd. Then set `ready_event_due = now` |

Then the new image replies on the adopted client fd
(`ok: re-executed into <version>, <n> services adopted`) and closes it. The client sees
the *new* version confirm itself, not the old one promising.

**Post-exec failure** (blob unreadable or overlay fails, which the dry run should have
made impossible): `fexecve(oldexe, {argv0, "--reexec", "<fd>"})`. The old image takes its
own state back. If that also fails, nothing in userspace can save PID 1. The kernel
panics on PID 1 exit and the hardware watchdog reboots the box. The dry run exists so
that this branch is never reached.

### 5. Not changed

SIGHUP stays config reload. `schema-ctl reload` behaviour is identical (it only switches
to `svc_runtime_copy`). No service sees any signal or fd change.

## Commit order (each TDD, `make test` green)

1. `SVC_RUNTIME_FIELDS` + `svc_runtime_copy`; `handle_reload` uses it. Behaviour-neutral.
   A test checks it carries the run-once timer and cgroup_path cases.
2. `reexec_state.h`: escape, `state_write`, `state_parse` + `tests/test_reexec_state.c`.
3. New-image path: `--reexec` / `--reexec-check` arg handling, boot-step skips, fd
   adoption, overlay, signal unblock, reply on the client fd.
4. Old-image path: `reexec` ctl verb, refusals, async dry-run child (pid + deadline in the
   main loop), commit-time re-check + fresh blob, signal
   block, CLOEXEC dance, exec failure restore.
5. `oldexe` fallback. `schema-ctl reexec [PATH]` help text (the client read already blocks
   with no timeout, so it waits out the dry run), and a real version string: `SCHEMA_INIT_VERSION`
   is still hard-coded `"0.1.3"` while the RPM is 0.4.0-1.972. The Makefile injects the
   RPM version + git hash, otherwise the confirmation reply proves nothing.

Estimated ~700 lines including tests. Later PR: RPM `%posttrans`.

## Verification

- **Unit:** the state round-trip/escape/version/truncation tests; `svc_runtime_copy` cases.
- **VM (schema-vmtest + idle2 harness):**
  - re-exec into the same binary 50× in a loop with services up. Every `child_pid`,
    `restarts=` and state is unchanged, no timer fires early, and boot-timing doesn't
    re-run.
  - kill a service during the swap: it is reaped and recovers in the new image.
  - `schema-ctl status` hammered from another shell during the loop never gets
    ECONNREFUSED.
  - SIGTERM sent mid-swap: the VM powers off cleanly.
  - i6300esb watchdog (`WD=1`): no reset across 50 re-execs.
  - refusals: a binary that exits 1 in `--reexec-check`, a modified `.svc`, an orphan,
    under pressure. Each one is refused, and PID 1 keeps running the old image.
  - fallback: a test-only binary that rejects the blob after exec → old image resumes
    with the same pids.
- **Hardware:** blakbox `schema-ctl reexec` 1.972 → branch build. Check
  `/proc/1/exe` sha = new, `voluntary_ctxt_switches` continues, Plasma session untouched,
  `schema-doctor` CLEAN. Then DBox/Eli/Optiplex.

## Risks

- **PID 1 dies after exec.** Mitigated by the dry run (same binary, same blob, same
  config) plus the oldexe fallback. Residual risk is a fault that only shows when running
  as PID 1. The hardware watchdog bounds the damage to one reboot.
- **A runtime field missed in the list.** This is the same class as the three reload
  bugs, now in one place. The 50× VM loop compares the full `schema-ctl status --json`
  before and after.
- **Format drift between versions.** The version gate plus ignore-unknown/default-missing
  rules mean old↔new both parse. A test feeds a v1 blob with an extra unknown key.
- **Signals in the window.** Covered by blocking them (step 3.2) and by a VM test.

## Open questions for Jonathan

1. **Modified `.svc` files:** refuse (recommended, the same rule as reload) or add
   `schema-ctl reexec --accept-modified` to adopt them? Refusing means a host with a
   pending `.svc` edit needs a reboot or a revert before it can re-exec.
2. **Auto re-exec on upgrade:** after re-exec has run on all four hosts by hand, should
   the RPM `%posttrans` call `schema-ctl reexec` when `/proc/1/comm` is `schema-init`?
   Recommended: yes, in a later PR. A refusal there just logs "reboot to apply" and the
   upgrade still succeeds.
