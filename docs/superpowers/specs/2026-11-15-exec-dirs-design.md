# runtime_dir= / state_dir= / cache_dir= / logs_dir= (audit 4.4)

## Problem
The importer fakes systemd's `RuntimeDirectory=` family with root
`exec_pre=+/usr/bin/install -d …` lines (`schema-import.py` `exec_dir_lines`).
That costs `exec_pre` slots (a unit over 8 is skipped), never removes the
runtime dir, and has to drop nested entries under `User=`, because a root
`install -d -o` through a parent the user owns follows a symlink planted there.

## Keys
- `runtime_dir=`, `state_dir=`, `cache_dir=`, `logs_dir=`: each a
  space-separated list of relative paths, repeatable, up to 8 per kind.
  A path must be relative, with no empty, `.` or `..` component and no `:`.
  Bad values are refused at parse.
- `runtime_dir_mode=`, `state_dir_mode=`, `cache_dir_mode=`, `logs_dir_mode=`:
  octal, default `0755`.
- Base: system `/run`, `/var/lib`, `/var/cache`, `/var/log`. In schema-user
  (euid ≠ 0): `$XDG_RUNTIME_DIR`, `$XDG_STATE_HOME`, `$XDG_CACHE_HOME` (else
  `~/.cache`), `$XDG_STATE_HOME/log`, as systemd's user manager does.

## Creation (child, before the root exec_pre lines)
- The user and group are resolved first (moved ahead of the hardening step;
  `getpwnam` works the same before the mount namespace).
- Walk from the base one component at a time: `openat(O_NOFOLLOW|O_DIRECTORY)`,
  `mkdirat` the missing ones. Intermediates are root-owned `0755`; the leaf is
  `fchown`ed to `user=`/`group=` and `fchmod`ed to the mode, through its fd.
  Nothing is followed, so nested paths under `User=` are safe and the
  importer stops dropping them.
- A path that exists as a non-directory, or a failed step, fails the spawn
  (exit 126, `[schema-init] <svc>: runtime_dir=X: <step>: <errno>`), as systemd
  fails the unit.
- Not done: systemd's recursive chown of an existing tree whose owner changed.

## Environment
`RUNTIME_DIRECTORY`, `STATE_DIRECTORY`, `CACHE_DIRECTORY`, `LOGS_DIRECTORY`:
the full paths, sorted, `:`-joined; an `env=` of the same name wins.

## Hardening
Nothing to add: the bases are never under `protect_system=`'s read-only
mounts, and schema-user (where `protect_home` would matter) has no mount
namespace. A `landlock_rw=` service lists the dirs itself.

## schema-user
It clears the environment it was started with; `XDG_STATE_HOME` and
`XDG_CACHE_HOME` are now kept for services, so the supervisor and its
services agree on the bases (found in testing: without it the dirs landed
under the passwd home).

## Runners
`exec_stop`/`exec_stop_post`/`exec_reload` see the same `*_DIRECTORY`
variables (systemd sets them for every command); they create nothing.

## Runtime dir removal
PID 1 removes the runtime dirs when the service is stopped for good: no live
child and state EXCISED (`schema-ctl stop`, restart policy says no, given up).
Kept across automatic respawns, DORMANT backoff, and a oneshot's PERFECT
(acts as `RemainAfterExit=yes`). The delete runs in a forked child (PID 1
never walks a tree): `openat`/`unlinkat` only, no symlink is ever followed.
`rt_made` (dirs exist from a spawn) rides reload and re-exec.

## Importer
`RuntimeDirectory=`, `StateDirectory=`, `CacheDirectory=`, `LogsDirectory=`
and their `*Mode=` map to the native keys; no `exec_pre` lines, no 8-slot
skip. `ConfigurationDirectory=` keeps today's `test -e || install -d` line
(systemd never re-owns an existing config dir). Specifiers are still expanded
by the importer. The `*_DIRECTORY` env lines it writes today go away (the
engine sets them).

## Tests
- C parse test: good/bad paths, modes, the 8 cap, reset on empty value.
- schema-user live test: nested runtime/state dirs created with mode, env set,
  a planted symlink in a parent is refused, runtime dir gone after stop, kept
  across a crash respawn.
- `tests/test_import_units.py`: native keys emitted; ssh `RuntimeDirectory=sshd`.
- PID 1 on blakbox: a `user=nobody` test .svc with nested `state_dir=` under
  `protect_system=full`: owner, mode, writable, runtime dir removed on stop.

## Not in scope
`ConfigurationDirectory=` native, `*Preserve=`, `dir:symlink` (systemd 254),
`dynamic_user` ownership (4.10).

## Review changes (PR #533)
- A symlink below the base is followed when its directory is owned by root
  (the user, in schema-user) and writable by no one else; otherwise refused.
- state/cache/logs creation failures log and continue (the importer's old
  `-install` never blocked a start); runtime failures still fail the spawn.
- The user is resolved early only for a service with dirs, so a `+` exec_pre
  that creates the user (sysusers) still works for the rest.
- `schema-ctl restart` empties the runtime dir (systemd's stop+start).
- The delete never crosses into another mount (st_dev + STATX_ATTR_MOUNT_ROOT).
- `*_dir_mode=0000` is a real mode; aside names are unique per call.
- Known: renaming/removing `runtime_dir=` on a running service, or evicting
  it on reload, leaves the old directory.
