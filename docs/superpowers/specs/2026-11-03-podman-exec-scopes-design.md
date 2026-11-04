# podman exec scopes — `podman exec` from a login shell

Status: **rev 3: Greg's review, the live run and /code-review folded in; implemented in PR #466.**
Rev 2 and rev 3 changes are listed at the end.
Author: Claire, 2026-11-03. Backlog item from the rootless podman phase
(`2026-10-27-rootless-podman-quadlet-design.md`, "known gaps").

## Why

Rootless Quadlet containers run under schema-user, inside
`user.slice/user-U.slice/user@U.service/<svc>/`. A login shell lives in
`user.slice/user-U.slice/session-N.scope`. Reproduced on blakbox 2026-11-03
(podman 5.8.9, cgroupfs manager, cgroup v2), Quadlet `scopetest.container`
FUNDAMENTAL under schema-user:

```
$ podman exec systemd-scopetest true
Error: crun: write to `/sys/fs/cgroup/user.slice/user-1000.slice/user@1000.service/
  scopetest/libpod-payload-<id>/cgroup.procs`: Permission denied: OCI permission denied
rc=126
```

cgroup v2 lets a process move from A to B only when the writer can write the
`cgroup.procs` of A and B's **common ancestor**. That ancestor is
`user-U.slice`, which root owns. `podman exec` cannot work from a login shell
today: not from Konsole, not over ssh. The e2e script works around it by
running its exec from a throwaway .svc (`tests/livetest/podman-quadlet-e2e.sh`
lines 24-29).

### What systemd does, and what podman already asks us for

Every rootless podman command whose current cgroup it does not own asks the
**user** systemd manager for a scope holding itself. Traced on blakbox with
strace, `podman info` sends on the session bus:

```
StartTransientUnit("podman-<pid>.scope", "replace",
    [Slice="user.slice", PIDs=[<own pid>], Delegate=true, DefaultDependencies=false], [])
```

The session shim (`scripts/schema-systemd1-session.py:183`) refuses every
ExecStart-less transient unit, so podman logs at debug level
`Failed to add podman to systemd sandbox cgroup: schema-init does not manage
transient scopes` and carries on in `session-N.scope`. The podman pause process
was created the same way and lives in `session-1.scope` on blakbox.

systemd --user cannot move a process out of `session-N.scope` either. It calls
the **system** manager's `AttachProcessesToUnit("user@U.service",
"/user.slice/podman-<pid>.scope", [pid])`, and PID 1 (root) does the write.

### Proven by hand (blakbox, 2026-11-03)

As the user: `mkdir user@1000.service/scopes.slice/podman-proof.scope`. As root:
write the shell's pid into its `cgroup.procs`. From that shell:
`podman exec systemd-scopetest sh -c 'echo exec-ok'` → `exec-ok`, rc=0. Once
both sides of the move are under `user@1000.service`, which the user owns, crun's
write is allowed. Nothing else is needed.

## Goal

`podman exec` (and anything else needing the same move) works from any login
shell that has a session bus, because schema-init does the same two steps as
systemd:

1. **The session shim** (runs as the user) accepts podman's scope request,
   creates the scope cgroup in the user's own tree, and asks the system side to
   move the pid there.
2. **The system bridge** (`scripts/schema-systemd1.py`, root) implements
   `AttachProcessesToUnit` for that single case, under strict checks.

## Non-goals

- General transient scopes. Chromium, KDE `app-*.scope` and every other
  ExecStart-less request stay refused, as today (the shim's comment at :195
  records why: ACKing Chromium's scope wedged its network service).
- `systemd-run --user --scope`, `Delegate=` controller enabling, scope
  properties (limits, `KillMode`), `StopUnit`/`KillUnit` on a scope, scope
  objects on the bus.
- Changing podman's cgroup manager (stays cgroupfs) or Quadlet's `--cgroups=split`.
- ssh-only users without a session bus. They keep today's behaviour (see Open
  questions).

## Anti-goals (a failure even if exec works)

- Any podman command fails, or gets noticeably slower, where it works today. A
  refusal or a broken step must leave podman exactly where it is now: run in
  place, log at debug level.
- A user can move a process they don't fully own, or a process into a cgroup
  outside their own `user@U.service`.
- Chromium tabs wedge again.
- The system bridge's existing `StartTransientUnit` ack (KDE app launches) changes.

## Design

### Names

- **scope name**: `podman-<suffix>.scope` with
  `^podman-[A-Za-z0-9_-]{1,64}\.scope$`. This covers `podman-<pid>.scope` and
  the pause process's `podman-pause-<n>.scope`. The pause request is refused in
  practice anyway: podman puts the pause pid in `PIDs`, not its own, and the
  PIDs==[sender] rule refuses that. This is from podman's source, not seen live.
- **scope dir**: `<CG>/user.slice/user-U.slice/user@U.service/user.slice/<scope name>`,
  where `<CG>` is `$SCHEMA_CGROUP_ROOT` or `/sys/fs/cgroup`, as in logind. The
  `user.slice` under `user@U.service` mirrors systemd's own layout, and podman
  asks for `Slice=user.slice`. A schema-user service cgroup is `user@U.service/<name>`,
  and `service.c:2322` does not validate `name=`, so a hand-written
  `name=user.slice` would collide. An imported unit can't, because the importer
  drops `.service`. Guard (my call): the shim refuses if `user.slice` looks
  like a schema-user service cgroup, meaning it holds processes itself or has a
  `runtime` / `libpod-payload-*` child (the `--cgroups=split` layout). Other
  children are expected. Rev 1 refused any non-scope child, and the live run
  showed that wedges every scope while an ad-hoc `podman run -d` container lives:
  podman puts that container's cgroup in `user.slice/<64-hex id>`.

### 1. Session shim: `StartTransientUnit` for podman scopes

In `handle_start_transient_unit`, before the existing ExecStart-less refusal:

Accept only if **all** of these hold:

- no `ExecStart` (an ExecStart request keeps the existing spawn path);
- the name matches the scope-name regex;
- `PIDs` is present and is exactly `[<sender pid>]`, where `<sender pid>` comes
  from the bus driver's `org.freedesktop.DBus.GetConnectionUnixProcessID(msg.get_sender())`
  (dbus-python's `Bus` has `get_unix_user` but no pid helper). podman
  always sends its own pid, and the sender is blocked waiting for the reply, so
  the pid can't be recycled meanwhile;
- `Slice` is absent or `user.slice`;
- the user's `user@U.service` exists and is owned by U (schema-user's
  delegation, `schema-logind.py:317`).

Anything else gets the existing `NotSupported` refusal, unchanged.

Accepted:

1. `mkdir -p` the `user.slice` dir (user-owned, so no root needed; no controllers
   enabled, because podman's own process needs none), then `mkdir` the scope
   dir. If the scope dir already exists and is unpopulated (pid reuse of a
   leftover), reuse it. If it is populated, refuse.
2. Call the system bus `org.freedesktop.systemd1.Manager.AttachProcessesToUnit(
   "user@U.service", "/user.slice/<scope name>", [pid])` with a 2 s timeout.
3. Success → `_reply_job_and_done` (job reply, then JobNew/JobRemoved "done";
   go-systemd waits for that JobRemoved). Failure or timeout → `rmdir` the scope
   dir and refuse with `NotSupported`, so podman continues in place as today.
4. Log one line each way: `scope podman-123.scope pid=123 -> attached` /
   `-> refused (<why>)`.

**Sweep:** empty scope dirs are removed by the shim. Each accepted request and a
60 s GLib timer remove every `user.slice/podman-*.scope` whose `cgroup.events`
says `populated 0`. If the shim dies, leftover empty dirs are harmless, and
logind's `remove_user_cgroup` (deepest-first) clears the tree on the next
schema-user start.

### 2. System bridge: `AttachProcessesToUnit(s unit, s subcgroup, au pids)`

A new `@dbus.service.method` on the Manager in `schema-systemd1.py`, with the
real systemd signature. The stock Fedora policy
`/usr/share/dbus-1/system.d/org.freedesktop.systemd1.conf` already allows
`send_member="AttachProcessesToUnit"` to everyone, so no policy file changes.
Authorisation happens in the method:

- **caller uid** C = `GetConnectionUnixUser(sender)`. Refuse C == 0 (root
  needs no help, and this keeps the surface user-only).
- `unit` must equal `"user@%d.service" % C` exactly.
- `subcgroup` must match `^/user\.slice/podman-[A-Za-z0-9_-]{1,64}\.scope$`
  (the same name rule, no `..`, no other shape).
- the target dir `<user@C.service><subcgroup>`: `user@C.service`,
  `user@C.service/user.slice` and the leaf are each `os.lstat`-checked: a real
  directory (not a symlink) with `st_uid == C`. Then
  `os.path.commonpath([user_svc, os.path.realpath(target)]) == user_svc`. Real
  cgroupfs refuses `symlink()` (EPERM, checked on blakbox), so this is defence in
  depth. It does matter for the plain-filesystem `SCHEMA_CGROUP_ROOT` used in tests.
- `pids`: 1 to 4 entries. For each pid:
  1. `fd = os.pidfd_open(pid)` (ESRCH → error);
  2. the `Uid:` line of `/proc/<pid>/status` must show C in all four fields
     (real, effective, saved, fs). This refuses setuid helpers and anything a
     user merely shares a session with. Gid is not checked: cgroup and DAC
     permission here are uid-bound, and a primary gid often differs from the uid
     (LDAP, `users`=100);
  3. `fd` must not poll readable. That proves the status read in step 2 belonged
     to the process `fd` holds, and that it is still alive;
  4. write the pid to the target's `cgroup.procs` (ESRCH → it exited → error);
  5. if `fd` now polls readable, return an error and log it. **Never move a pid
     anywhere else** to "undo" the write: a recycled pid was never in the old
     cgroup, so writing it there would hijack an innocent process (Greg #1).
  Residual window: step 3 to step 4, a single syscall pair. Exploiting it would
  need the caller's process to exit and its number to come round again through
  `pid_max` (4,194,304 on blakbox) inside that gap. Accepted, as systemd's
  pid-number API accepts it.
- Any refusal → `org.freedesktop.DBus.Error.AccessDenied` (identity checks) or
  `InvalidArgs` (shape), and nothing is written.
- Log one line per call: `AttachProcessesToUnit(user@1000.service,
  /user.slice/podman-123.scope, [123]) uid=1000 -> ok`.

Sandbox: the bridge runs with `protect_system=full`, which makes `/usr /boot
/efi /etc` read-only and leaves `/sys` alone (`ns.c:41`). The write needs root's
DAC override, which it has (no `caps=` line in `schema-systemd1.svc`). The
builder checks this live, not just on paper.

### 3. What changes for each kind of podman process

| Caller | Owns its cgroup? | Before | After |
|---|---|---|---|
| login shell `podman exec` / `ps` / `info` | no | runs in session-N.scope, exec EACCES | moved to `podman-<pid>.scope`; exec works |
| pause process (first podman after boot) | no | session-N.scope (dies with the session) | unchanged: its request names the pause pid, not the sender, so it is refused |
| Quadlet podman under schema-user | yes | no scope request | no scope request (unchanged; must be verified, see Tests) |
| ad-hoc `podman run` from a shell | no | runs in session-N.scope, container gets no cgroup | **changes (seen live 11-03):** conmon stays in `podman-<pid>.scope` (populated, so the sweep leaves it alone), and the payload gets its own `user.slice/<container id>` cgroup, which podman removes on `rm`. Whether `--memory` now holds for ad-hoc containers is unmeasured |

The process leaves `session-N.scope`, so `sd_pid_get_session()` no longer
reports a session for it. systemd has the same property, and podman doesn't
need a session. The login shell itself never moves.

Lifetime: scopes live inside schema-user's tree. When schema-user crashes or
hits its stop timeout, logind's `kill_user_cgroup` kills them. Before rev 3 an
*ordered* stop left them running unsupervised until the next `_start`, so logind
now kills a populated tree after an ordered stop too. That includes a
`podman exec -it` shell and an ad-hoc `podman run -d` conmon, as systemd's
`user@U` stop does. This is the only logind change.

## Tests

Unit (`tests/test_systemd1_session.py`, `tests/` for the bridge, with a fake
cgroup root via `SCHEMA_CGROUP_ROOT`, as the logind tests do):

- shim accepts the exact podman payload; refuses: wrong name (`app-x.scope`,
  `chrome-1.scope`), PIDs ≠ sender, two PIDs, `Slice=foo.slice`, ExecStart
  present (goes to spawn path), tree not owned by U, populated existing dir.
- shim on bridge error/timeout: dir removed, `NotSupported` returned.
- sweep removes `populated 0` dirs only, and only `podman-*.scope`.
- bridge refuses: root caller; `user@<other>.service`; `/user.slice/../x`,
  `/foo.scope`, symlinked target; target not owned by C; pid owned by another uid;
  pid with euid ≠ ruid; more than 4 pids; symlinked `user.slice` (fake root).
  Gid ≠ uid but Uid all C → accepted. Race path: pidfd readable before the
  write → no write, error; readable after → error, and nothing moved anywhere
  else.

Live (`tests/livetest/podman-exec-scope.sh`, blakbox, after COPR upgrade +
relogin: the shim is session-bus-activated, so a relogin picks it up):

1. Quadlet up → `podman exec <c> true` from a Konsole shell, rc=0.
2. During `podman exec <c> sleep 3`, `user@U.service/user.slice/podman-*.scope`
   exists and holds podman; within 60 s of exit it is gone.
3. `busctl --user call ... StartTransientUnit chrome-1.scope ...` → NotSupported
   (Chromium path unchanged). Open Helium/Chromium, load two pages.
4. As the user, call system-bus `AttachProcessesToUnit` with pid 1 →
   AccessDenied; with `user@0.service` → AccessDenied; with `/user.slice/../..` →
   InvalidArgs.
5. Starting a Quadlet logs **no** scope request in `schema-stu.log`.
6. `podman run --rm debian:trixie true` and `podman run -d --name t ... sleep 60`
   from the shell both still work. Record where `t`'s cgroup lands.
7. `time podman ps` before and after: the added cost is under 50 ms.
8. Mutation check: with the bridge method stubbed to fail, exec still fails the
   old way and `podman ps` still works.
9. Update `podman-quadlet-e2e.sh`: drop the e2e-hog workaround and run the hog
   with `podman exec` directly from the shell (check 6 stays).

## Where it goes (sizing)

One PR, Python only: shim ~80 lines, bridge ~70, tests ~150, live script, e2e
edit, `docs/svc-reference.md`/roadmap line. No C, no PID 1 change, no
re-exec concerns. Deploy = package upgrade + relogin. The bridge is a
schema-init service, and restarting it is fine; it is not a session daemon.

## Open questions (for Greg / Jonathan)

1. **ssh-only sessions.** The shim is activated on the session bus. Is there a
   session bus at `/run/user/U/bus` for an ssh login with no desktop, on DBox
   (linger + ssh)? If not, exec over ssh stays broken. That's acceptable for v1
   and gets its own follow-up if wanted.
2. ~~Race-restore~~: settled in rev 2. Poll before and after the write, never
   restore (Greg #1).
3. ~~Restrict the bridge to the shim~~: settled as no. Greg agreed: `exe` is
   `python3`, and the checks already bind to the uid.

## Follow-ups (not in this PR)

- `service.c` doesn't validate `name=`. Rejecting `*.slice`, `*.scope` and
  `init.scope` would remove the `user.slice` collision outright. The shim's
  guard is a heuristic: a hand-written `name=user.slice` service that hasn't
  started yet slips past it. That fix touches C and PID 1, so it is left out of
  this Python-only PR.
- The shim's two bus calls block its GLib loop for up to 2 s each when the
  bridge or bus driver is slow. An async version is possible if that ever shows
  up live.

## Rev 3 changes (live run + /code-review)

1. The guard against a service named `user.slice` now detects a service
   layout, not "any non-scope child". An ad-hoc `podman run -d` puts its
   container cgroup in `user.slice/<id>` (seen live), and the old rule wedged
   every scope while it lived.
2. Bridge: the source cgroup must be under `/user.slice/user-C.slice/`. A
   system service running as the user's uid can no longer be pulled out of
   root's limits and kill.
3. logind kills a populated user tree after an ordered schema-user stop.
4. Shim: every error answers podman (`NotSupported`) and none escapes the
   filter; the sender-pid call has the 2 s timeout too.
5. Pause scope: documented as refused (see Names).
6. Rejected: closing the poll→write race with SIGSTOP or a re-check. A stopped
   process can still be killed, and a re-check comes after the move. The
   window stays documented and accepted.

## Rev 2 changes (Greg's review)

1. Race: dropped the move-back of a recycled pid, which would have hijacked a
   foreign process. Now: poll before the write, ESRCH on the write, poll after,
   return an error. Greg's "eliminates the window" overstated it: a
   one-syscall gap remains, documented and accepted.
2. Gid check removed. Rev 1 compared Gid against the uid, which would refuse
   every user whose primary gid ≠ uid. Uid only, all four fields.
3. Every path component is lstat-checked, plus a realpath/commonpath check.
   Not exploitable on real cgroupfs (no symlinks), but it holds the line in
   the test root.
4. The sender pid comes from the bus driver's `GetConnectionUnixProcessID`;
   dbus-python has no helper for it (confirmed).
5. ssh-only (Q1): stays a v1 gap. Greg says headless ssh logins start no
   broker. That is not verified here, so it stays a live check on DBox.
