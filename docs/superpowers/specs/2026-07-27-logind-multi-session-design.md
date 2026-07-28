# logind multi-session — design

**Date:** 2026-07-27
**Status:** DESIGN — not built. Groundwork landed on `feat/logind-session-properties` (`5da80ac`, `126e3b7`).
**Depends on:** nothing new; supersedes the hardcoded session identity in `scripts/schema-logind.py` and `/usr/local/bin/sddm-logged`
**Track:** B (systemd-compat surface — "indistinguishable from systemd")

## Problem

schema-init's logind shim serves exactly one session, and its identity is the literal string `31`.

That literal is a **contract between two files that must agree**, which is the real blocker:

- `scripts/schema-logind.py` — serves D-Bus objects at a fixed `/org/freedesktop/login1/session/_31`.
  As of `126e3b7` the literal is collected into `SESSION_ID` / `SESSION_PATH`, but it is still one hardcoded session.
- `/usr/local/bin/sddm-logged` — the login script. It creates the cgroup scope and writes the state file:

  ```sh
  # id 31 matches schema-logind's hardcoded session id. Failsafe; never blocks login.
  SESSION_SCOPE=/sys/fs/cgroup/user.slice/user-1000.slice/session-31.scope
  printf '%s\n' '# This is private data. Do not parse.' UID=1000 USER=ajax80 ACTIVE=1 \
      IS_DISPLAY=1 STATE=active REMOTE=0 SEAT=seat0 VTNR=1 TYPE=wayland CLASS=user DESKTOP=KDE \
      > /run/systemd/sessions/31
  ```

  Note `UID`, `USER`, `VTNR`, `TYPE`, `CLASS`, `DESKTOP` are hardcoded **here too** — this file is correct only for
  ajax80 on tty1 running KDE Wayland.

Consequences today:

- A second concurrent login (another VT, another user, a tty login alongside the GUI) is invisible. It gets no session
  object, no state file, and **no `session-<id>.scope` cgroup — so polkit rejects its auth agent with ENXIO**, which is
  exactly the failure documented in the `sddm-logged` comment.
- Every getty/tty login on `tty2`–`tty6` currently creates **no session at all**.
- `/run/systemd/seats/` and `/run/systemd/users/` are created but left **empty** (verified 2026-07-27). They exist only
  so `sd_login_monitor_new(NULL)` does not return `-ENOENT` and abort WirePlumber's logind module. Nothing reads real
  seat or user state from disk.

## Goal

Represent an arbitrary number of concurrent sessions across seats and users, over both D-Bus **and** the
`/run/systemd/**` state files, with correct `Active` tracking on VT switch.

Non-goals (v1): remote/ssh sessions, `CLASS=greeter` lifecycle, user lingering, `KillUserProcesses`, multi-seat with
more than one physical seat. `seat0` remains the only seat.

## Constraints / facts established

- **`sd_session_*` reads the files, not D-Bus.** `sd_session_is_active()` / `get_uid()` / `get_seat()` parse
  `/run/systemd/sessions/<id>`. polkit uses these. So the state files are not a mirror of the D-Bus surface — for a large
  class of consumers they *are* the interface. Both must be produced.
- **The cgroup scope is load-bearing.** `sd_pid_get_session()` is cgroup-based and resolves a session by walking to
  `/user.slice/user-<uid>.slice/session-<id>.scope`. Without it the polkit auth agent cannot register, which is what
  broke GUI privilege escalation before `sddm-logged` started creating it.
- **utmp is not a usable source.** `who -a` on blakbox reports `ajax80 ? seat0` with a 1969 epoch timestamp — sddm does
  not write a well-formed utmp record here. Do not derive sessions from it.
- **The login script is where a session is actually born.** It already creates the scope and writes the state file. It is
  the only place that knows a login happened at the moment it happens.
- **The bridge already polls.** `poll_active_vt()` runs every `VT_POLL_MS` (250ms) and is the existing mechanism for
  noticing VT changes.
- **This codebase has been bitten twice by fd-driven event loops** — the pidfd leak (`EMFILE` → busy-spin) and the
  half-open peer wedge (100% CPU for 11 days, starving PipeWire's RT thread). That history informs the watch mechanism
  chosen below.

## Design

**Ownership: the login path allocates, the bridge reads.** The login script is the writer of record for
`/run/systemd/sessions/<id>` and the scope cgroup. `schema-logind` treats that directory as its source of truth and
projects it onto D-Bus. Neither side hardcodes an id.

### 1. ID allocation (login path)

Allocate the lowest free positive integer by **atomic create**, so two simultaneous logins cannot collide:

```sh
alloc_session_id() {
    i=1
    while [ $i -lt 1000 ]; do
        if (set -o noclobber; : > "/run/systemd/sessions/$i") 2>/dev/null; then
            echo "$i"; return 0
        fi
        i=$((i + 1))
    done
    echo 31   # failsafe: never block login
}
```

`set -o noclobber` makes `>` fail if the file exists, which is the shell's `O_EXCL`. The failsafe returning `31`
preserves today's behaviour if anything goes wrong — **`sddm-logged` must never block a login**.

### 2. State file schema

Written by the login path. Keys are those `sd_session_*` actually parses, plus what the D-Bus surface needs:

```
# This is private data. Do not parse.
UID=<uid>          USER=<name>        ACTIVE=<0|1>       STATE=<active|online|closing>
SEAT=seat0         VTNR=<n>           TYPE=<wayland|x11|tty>
CLASS=<user|greeter>                  DESKTOP=<KDE|X-Cinnamon|GNOME|>
IS_DISPLAY=<0|1>   REMOTE=0           LEADER=<pid>       SERVICE=<sddm|login>
REALTIME=<usec>    MONOTONIC=<usec>
```

All values **runtime-detected**, never hardcoded — the same lesson that produced `get_session_type()` and
`get_desktop_name()`. `LEADER` is the login script's own `$BASHPID` (it is the session leader, which is what
`getsid()` resolves to today — verified: `getsid(kwin_wayland)` → `2185` = `sddm-logged`).

Companion files, currently empty directories:

- `/run/systemd/seats/seat0` — `ACTIVE=<id>`, `ACTIVE_UID=<uid>`, `SESSIONS=<id id …>`, `UIDS=<uid …>`
- `/run/systemd/users/<uid>` — `NAME=`, `STATE=`, `SESSIONS=`, `SEATS=seat0`, `DISPLAY=<id>`

These are derived, so **`schema-logind` writes them**, not the login script — it is the only component with the whole
picture.

### 3. Bridge: read, project, watch

Replace the single `Login1Session` instance with a registry.

```
SessionRegistry
  scan()      -> parse /run/systemd/sessions/* into records
  sync()      -> diff against live D-Bus objects:
                   new file  -> construct Login1Session(bus, sid, rec), emit Manager.SessionNew(id, path)
                   gone file -> obj.remove_from_connection(), emit Manager.SessionRemoved(id, path)
                   changed   -> update record, emit PropertiesChanged for the changed keys only
                 then rewrite the derived seat/user files and refresh Login1User objects
```

**Watch mechanism: extend the existing 250ms poll, do not add an inotify fd.** `poll_active_vt()` already runs on that
cadence; `sync()` hangs off the same timer and stats the directory. This is deliberately the boring choice — an added
`Gio.FileMonitor` or raw inotify fd is a new long-lived descriptor in a GLib loop, and that is precisely the shape of
both prior CPU-spin incidents. A `readdir` of a directory holding single-digit numbers of entries at 4Hz is free.
Revisit only if profiling says otherwise.

Objects become per-id: `/org/freedesktop/login1/session/_<id>`, `/org/freedesktop/login1/user/_<uid>`.
`SESSION_PATH` from `126e3b7` becomes a function of the id.

### 4. Active tracking

On VT change, exactly one session is active: the one whose `VTNR` equals the active VT.

```
on_vt_changed(new_vt):
    for each session:
        want = (session.vtnr == new_vt)
        if want != session.active:
            session.active = want
            rewrite ACTIVE=/STATE= in its state file
            PropertiesChanged(Session, {Active, State})
            pause/resume its taken devices + DRM master   # existing logic, now per-session
    Seat.ActiveSession = the active one
    PropertiesChanged(Seat, {ActiveSession, ActiveSessionId})
    rewrite /run/systemd/seats/seat0
```

The existing device-handoff logic in `on_vt_changed()` already does the DRM-master half correctly for one session; it
moves inside the per-session loop unchanged.

### 5. Manager surface

- `ListSessions` / `ListUsers` / `ListSeats` — from the registry rather than a fixed tuple.
- `GetSession(id)`, `GetUser(uid)`, `GetSessionByPID(pid)`, `GetUserByPID(pid)` — resolve PID via the
  `session-<id>.scope` cgroup line in `/proc/<pid>/cgroup`, falling back to `getsid()`.
- New signals: `SessionNew`, `SessionRemoved`, `UserNew`, `UserRemoved`, `SeatNew`, `SeatRemoved`.
- `LockSession`/`UnlockSession` (added in `126e3b7`) route to the named session instead of the only one.

### 6. Logout

`sddm-logged` loops (`while true`) and relaunches the session. Each iteration must clean up before allocating again:
remove `/run/systemd/sessions/<id>`, `rmdir` the scope. Add a `trap` so an abnormal exit does not leak a stale file —
a stale file means a phantom session on D-Bus forever.

## Deployment order and backward compatibility

The two files deploy independently, so the bridge **must tolerate an un-updated login script**:

> If `/run/systemd/sessions/` is empty, synthesise the legacy single session `31` from runtime detection, exactly as
> today.

That makes the order irrelevant and every step individually revertible:

1. Ship the bridge (registry + legacy fallback). Behaviour identical on the current box, because `sddm-logged` still
   writes `31` and the registry finds it.
2. Ship `sddm-logged` allocation. Now ids are real.
3. Add the same snippet to the getty wrapper so tty logins get sessions (`TYPE=tty`, `CLASS=user`).

## Risks

- **`sddm-logged` can block login.** Highest-consequence file on the box. Every added line keeps the `2>/dev/null || true`
  failsafe idiom already used there, and the id allocator falls back to `31`. Recovery is the existing
  `.bak-YYYYMMDD` convention — three baks already sit beside it.
- **Stale state files = phantom sessions.** Mitigated by the `trap` cleanup, plus a startup sweep that drops any session
  file whose `LEADER` pid is no longer alive.
- **Restart budget.** `schema-logind` hit `restarts=5` on 2026-07-27; repeated restarts trip `DORMANT`
  (clear with `schema-ctl reset schema-logind`). Validate under **vmtest**, not live restarts.
- **polkit regression.** If the scope cgroup and the state file disagree on the id, GUI privilege escalation breaks
  silently. Test flatpak/Discover/udisks auth explicitly after any change here.

## Test plan (vmtest)

1. GUI login only → `loginctl list-sessions` shows 1 with a real id, all columns populated.
2. GUI + tty2 login concurrently → 2 sessions, distinct ids, distinct `VTNR`.
3. Ctrl-Alt-F2 → `Active` flips on both, `Seat.ActiveSession` follows, `PropertiesChanged` observed on both objects.
4. Log out of tty2 → file removed, `SessionRemoved` emitted, object gone from the bus, no phantom in `list-sessions`.
5. `sd_session_is_active()` via ctypes agrees with D-Bus for every session.
6. polkit: GUI auth agent registers in both sessions; a privileged Discover/udisks action prompts and succeeds.
7. Second *user* logging in on tty3 → own `Login1User` object, own `/run/systemd/users/<uid>`.
8. Kill a session leader with `-9` → startup sweep and/or `trap` removes the phantom.

## Open questions

- **`CLASS=greeter` for sddm itself.** Real logind gives the greeter its own session. Worth it, or does it just add a
  session nothing consumes?
- **`Display` property.** Left empty for Wayland in `5da80ac`. Should it carry the Wayland socket name, or stay
  X11-only as real logind does?
- **`IdleHint` aggregation.** Manager-level `IdleHint` is the AND/OR of per-session hints — which, and does anything
  here actually read it?
