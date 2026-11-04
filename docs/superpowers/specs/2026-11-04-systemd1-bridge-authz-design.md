# System bridge caller authorization

## Problem

Fedora's stock `org.freedesktop.systemd1.conf` lets any user send `StartUnit`,
`StopUnit`, `Reload` and others, because systemd checks polkit inside the
daemon. `schema-systemd1.py` had no such check and ran `schema-ctl` as root, so
any local user could start, stop or restart any service. Proven on DBox
(2026-11-04): `sudo -u nobody busctl call ... StopUnit ss crond.service replace`
stopped crond.

## Design

`authorize()` decides each mutating call:

| Caller | Result |
|---|---|
| No sender (`/run/systemd/private`, mode 0600) | allowed |
| uid 0 | allowed |
| Anyone else | asks polkit `CheckAuthorization`, async |
| polkit missing, errors, or uid lookup fails | `AccessDenied` |

Refused calls answer `AccessDenied`, or `InteractiveAuthorizationRequired` when
polkit would have challenged a client that didn't allow interaction. Polkit may
prompt only when the client set the allow-interactive flag, as systemd does, so
background desktop callers can't raise password dialogs.

| Methods | Polkit action |
|---|---|
| `StartUnit` `StopUnit` `RestartUnit` `TryRestartUnit` `ReloadUnit` `ReloadOrRestartUnit` `ReloadOrTryRestartUnit` | `manage-units` |
| `Reload` | `reload-daemon` |
| `SetEnvironment` `UnsetEnvironment` `UnsetAndSetEnvironment` | `set-environment` |

Left open because they change nothing: reads, and the no-ops (`KillUnit`,
`SetUnitProperties`, `EnableUnitFiles`, `MaskUnitFiles` and so on).
`StartTransientUnit` only acknowledges a job. `AttachProcessesToUnit` has its
own validation.

Every gated call logs the caller's uid and the outcome.

Polkit gets `unit` and `verb` in its details, as from systemd, so unit-scoped
rules work. `unit` is the name `schema-ctl` will really act on: it strips the
suffix, so `sshd` and `sshd.target` both reach sshd and both read as
`sshd.service`.

## Session shim

A session-bus caller means its user manager, and the shim is it. `Reload`, the
environment calls and `Start`/`Stop` of a `.target` have nothing to change on
the user side, so the shim acknowledges them. Forwarded, they acted on the
system manager (one user's environment for everyone, a full system reload). A
real system unit still forwards for polkit, but never interactively: the forward
blocks the shim's only thread, so a dialog would freeze every app launch in the
session (seen on Eli). Real systemd sends none of these to the system manager.

## Rail and migrate

With the bridge gated, `schema-systemd1.svc` joins the ISO rail and migrate's
`prevent-set.list`, because `podman exec` from a login shell needs its
`AttachProcessesToUnit`. Existing boxes keep their service set until someone
enables it.

No `dep=polkitd`: the rail's `plasma-autologin` already waits for polkitd, root
never needs it, and the dependency would cost root its bridge if polkitd failed.

## Verified live

Eli, real polkit: `nobody` and the unauthenticated desktop user refused with
crond's pid unchanged, including `crond` and `crond.target` spellings; reads,
root and the private socket work; a forged log line stays escaped.

Eli, session bus: `SetEnvironment`, `Reload`, target start/stop and
`dbus-update-activation-environment --systemd` succeed without reaching the bridge.

DBox, bridge enabled with the session shim: `podman-exec-scope.sh` 10/10 with
`cgroup_manager = "cgroupfs"`; exec from ssh and with no session bus work; a
desktop logout makes no gated calls.

## Known limits

- `podman run` fails on any box without `cgroup_manager = "cgroupfs"` (crun's
  scope request is refused). blakbox has that setting by hand; no package ships
  it. Separate from this change.
- A polkit answer that arrives after the client gave up still acts, and a prompt
  can't be withdrawn. Someone authenticated on purpose, so this stays simple.
- A real dialog was seen to start on Eli (`polkit-agent-helper-1`) but nobody
  typed a password in it, so an authenticated non-root call is untested live.
