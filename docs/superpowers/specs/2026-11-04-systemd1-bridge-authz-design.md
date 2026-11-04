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

## ISO rail

With the bridge gated, `schema-systemd1.svc` joins the ISO rail, because
`podman exec` from a login shell needs its `AttachProcessesToUnit`. Existing ISO
boxes keep their service set until someone enables it.

## Verified live

Eli, real polkit: `nobody` and the unauthenticated desktop user refused with
crond's pid unchanged; reads, root and the private socket work.

DBox, bridge enabled with the session shim: `podman-exec-scope.sh` 10/10 with
`cgroup_manager = "cgroupfs"`; exec from ssh and with no session bus work; a
desktop logout makes no gated calls.

## Known limits

- `podman run` fails on any box without `cgroup_manager = "cgroupfs"` (crun's
  scope request is refused). blakbox has that setting by hand; no package ships
  it. Separate from this change.
- A polkit prompt needs an agent; the interactive path was only checked for the
  flag reaching polkit, not a real dialog.
