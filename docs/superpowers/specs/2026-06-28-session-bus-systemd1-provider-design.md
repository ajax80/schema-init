# Session-bus `org.freedesktop.systemd1` provider — design

**Date:** 2026-06-28
**Status:** BUILT (Option A) 2026-06-28 — `scripts/schema-systemd1-session.py`. Live + activation-verified on blakbox.
**Depends on:** `2026-06-20-schema-systemd1-dbus-design.md` (the system-bus bridge `schema-systemd1.py`)
**Track:** B (systemd-compat surface — "indistinguishable from systemd")

## Problem

`org.freedesktop.systemd1` is served on **two** D-Bus buses on a normal systemd desktop:

- **System bus** — owned by PID 1 (systemd). On schema-init this is provided by `schema-systemd1.py` (the bridge that polls `schema-ctl status --json`).
- **Session bus** — owned by the per-user `systemd --user` manager. **schema-init has no `systemd --user`, so nothing owns this name on the session bus.**

Any session-bus client that asks for `org.freedesktop.systemd1` therefore hits the stock systemd D-Bus *activation* file
(`/usr/share/dbus-1/services/org.freedesktop.systemd1.service` → `Exec=/bin/false`), which exits 1:

```
org.freedesktop.DBus.Error.Spawn.ChildExited: Process org.freedesktop.systemd1 exited with status 1
```

**Found via:** Ferrix System Monitor's "System Manager" tab (2026-06-28). Ferrix (zbus) queries the **session** bus for the
systemd manager. Reproduced verbatim against Ferrix's own session bus (`/tmp/dbus-<id>`). This is the same class of gap as
the system-bus surface — just on the other bus. Other affected clients: gnome-system-monitor, mission-center, any GUI that
talks to the user manager.

Note: this is **separate** from the 2026-06-28 `schema-ctl` 4KB read-truncation bug (which broke the *system*-bus surface by
serving zero units). That is fixed (master `710cbf3`). The session-bus name has simply never had a provider.

## Goal

Serve a working `org.freedesktop.systemd1` on the **user session bus** so session-bus GUI clients enumerate and (where
permitted) manage schema-init services, with no app-side configuration — Track-B transparency extended to the session bus.

Non-goals (v1): a full `systemd --user` user-service manager (per-user units, user `.service` files). This spec is only the
**manager name on the session bus**, backed by the existing system-bus service model.

## Constraints / facts established

- The session bus is **per-user and ephemeral**: address in `DBUS_SESSION_BUS_ADDRESS`, created at login
  (here `dbus-launch` style `unix:path=/tmp/dbus-XXXX`, *not* `/run/user/1000/bus` — because there is no `systemd --user`
  to provide the `/run/user` bus). A root daemon started at boot does **not** know this address.
- **A non-root user can already read the system-bus `org.freedesktop.systemd1` surface** (verified 2026-06-28 as `ajax80`:
  `Manager.GetUnit`, `ListUnits`, `Properties.GetAll` all return without privilege). So a provider running **inside the user
  session** can forward read queries to the system-bus bridge with no extra privilege.
- Write methods (`StartUnit`/`StopUnit`/`RestartUnit`/…) on the system bus are gated by polkit (already handled by the
  bridge); forwarding them preserves that gate.

## Options considered

**A. Per-session forwarding relay (RECOMMENDED).**
A small process that runs **in the user session**, requests `org.freedesktop.systemd1` on the session bus, and forwards every
call on `/org/freedesktop/systemd1[/...]` to the system-bus `org.freedesktop.systemd1`, returning the reply. Re-emits the
bridge's signals (`UnitNew`/`UnitRemoved`/`PropertiesChanged`/`JobNew`/`JobRemoved`) session-ward.
- + Single source of truth (the system-bus bridge); no duplicated state/polling.
- + Runs as the user → naturally has the session bus; read-forwarding needs no privilege; writes keep polkit.
- + Naturally multi-session (one relay per session).
- − Requires a generic D-Bus message proxy (own a name, blind-forward arbitrary methods + introspection + signals).

**B. Root bridge also attaches to session buses.**
`schema-systemd1.py` discovers active session-bus addresses and claims the name on each.
- − Root owning a name on a user's private bus; dynamic discovery of ephemeral addresses across login/logout; fragile.
  **Rejected.**

**C. Real `systemd --user` equivalent (schema-init user manager).**
- − Large; this is its own future project. Out of scope. The relay (A) is forward-compatible: drop it once a real user
  manager exists.

## Recommended design (Option A)

### Activation, not autostart
Provide the name **on demand** via session D-Bus activation rather than an always-on autostart process:

- Ship a session service file that overrides the stock `/bin/false` one. User-level override dir
  (`~/.local/share/dbus-1/services/org.freedesktop.systemd1.service`) takes precedence over `/usr/share`, or install
  system-wide to `/usr/local/share/dbus-1/services/` (precedence over `/usr/share`). Contents:
  ```
  [D-BUS Service]
  Name=org.freedesktop.systemd1
  Exec=/usr/local/bin/schema-systemd1-session
  ```
- When a client (Ferrix) first calls the name, the session dbus-daemon spawns the relay **inside the session** (so it
  inherits the correct `DBUS_SESSION_BUS_ADDRESS`). The relay requests the name, services the call, and stays resident to
  serve subsequent calls + push signals.
- ⚠️ Packaging gotcha: the override must out-rank the systemd package's file on the session-service search path; verify with
  `dbus-send`/`busctl --user` after install. Do **not** edit the systemd-owned file in `/usr/share` (clobbered on update).

### The relay (`schema-systemd1-session`)
Implementation: dbus-python + GLib (consistent with `schema-systemd1.py`), or a thin zbus/sd-bus equivalent.

Core: a low-level message proxy.
1. Connect to session bus; `RequestName("org.freedesktop.systemd1")` (no-queue, fail if already owned).
2. Connect to system bus.
3. Install a message filter for destination self / path prefix `/org/freedesktop/systemd1`:
   - Rewrite destination → `org.freedesktop.systemd1` on the **system** bus, forward, await reply, relay reply (or error,
     including the polkit `auth_admin` flow for writes) back to the session caller. Preserve serial/sender semantics.
   - Handle `org.freedesktop.DBus.Introspectable.Introspect` and `Properties.Get/GetAll` the same way (blind-forward) so
     clients that introspect first (most GUIs) work.
4. Subscribe to system-bus signals from `org.freedesktop.systemd1` and re-emit them on the session bus.
5. Idle-exit after N seconds with no clients (optional; activation re-spawns on demand).

### Minimal-effort fallback (if the generic proxy is too much for v1)
Re-export, don't forward: run a session-bus instance of the **bridge's own object model** but source its data from the
system-bus bridge (`ListUnits` + per-unit `GetAll`) instead of `schema-ctl` (which needs root for the control socket).
Implements only the subset GUIs actually use: `Manager.ListUnits`, `GetUnit`, `Subscribe`, the boot-timestamp properties
(`FirmwareTimestampMonotonic`, `LoaderTimestampMonotonic`, `KernelTimestamp`, `UserspaceTimestamp`, `FinishTimestamp`),
`SystemState`, `Version`, and `Unit`/`Service` object props. Less faithful, but smaller and each method is testable.

> Open question for the implementer: the system-bus bridge's `Manager.GetAll` currently returns only
> `Version`/`SystemState`/`Features`/`Architecture` (verified 2026-06-28). The boot-timing properties Ferrix shows
> ("Startup finished in firmware + loader + kernel + userspace") are **not** exposed yet. Whichever path is taken, those
> Manager timestamp properties must be added (to the bridge, then forwarded/re-exported) for the boot-time readout to work.

## Security

- Relay runs **as the user**, on the user's **own** session bus — no privilege escalation introduced.
- Reads forward to a surface the user can already read directly.
- Writes forward to the system bus where polkit still adjudicates (`org.freedesktop.systemd1.manage-units`, etc.).
- Validate/whitelist the forwarded interface + path prefix so the relay can't be used to pivot to unrelated system-bus
  destinations (only `org.freedesktop.systemd1` + `/org/freedesktop/systemd1/**`).

## Testing

1. **Repro baseline:** `dbus-send --session --dest=org.freedesktop.systemd1 … Manager.Get Version` → `Spawn.ChildExited` (pre-fix).
2. **Post-install:** same call returns `"256"`; `busctl --user list` shows the name owned by the relay (not activation-failed).
3. **Ferrix:** System Manager tab loads units + boot timing (no error dialog).
4. **gnome-system-monitor / mission-center:** services list populates.
5. **Write path:** a `RestartUnit` from a session client triggers the polkit prompt and succeeds/denies correctly.
6. **Signals:** start/stop a service via `schema-ctl`; session clients see live `UnitNew`/`PropertiesChanged`.
7. **Lifecycle:** log out / log in → activation re-spawns the relay on first query (no stale `/tmp/dbus-*` assumptions).

## Rollout / rollback

- Ship: the activation override file + `/usr/local/bin/schema-systemd1-session`. No change to the system-bus bridge except
  adding the Manager timestamp properties.
- Rollback: remove the override file (session bus reverts to the `/bin/false` stub = today's behavior) and the binary.
  Zero impact on the system-bus surface or PID 1.

## Build notes (as implemented 2026-06-28)

Implemented Option A (the generic forwarding relay), not the minimal-effort fallback.

- **Relay:** `scripts/schema-systemd1-session.py` → installed `install -m0755 … /usr/local/bin/schema-systemd1-session`.
  dbus-python low-level message filter: owns `org.freedesktop.systemd1` on the session bus (`NAME_FLAG_DO_NOT_QUEUE`),
  blind-forwards any MethodCall under `/org/freedesktop/systemd1` (and `/`) to the system-bus surface via
  `send_message_with_reply_and_block`, relays the reply/error, and re-emits the system bus's signals session-ward.
- **Activation (per-user, highest precedence):** `~/.local/share/dbus-1/services/org.freedesktop.systemd1.service`:
  ```
  [D-BUS Service]
  Name=org.freedesktop.systemd1
  Exec=/usr/local/bin/schema-systemd1-session
  ```
  XDG_DATA_HOME outranks `/usr/share` so this beats the stock `/bin/false` stub. For a multi-user box, install to
  `/usr/local/share/dbus-1/services/` instead (still ahead of `/usr/share` in default `XDG_DATA_DIRS`).
- **Verified:** killed any owner → `ReloadConfig` → cold call activated the relay on demand (PID from the installed path);
  `Manager.Version`=256, `ListUnits`=38, `GetUnit`, `Subscribe`, unit `GetAll` (Id/LoadState/ActiveState/SubState),
  `Service.MainPID` all forward correctly. Complex nested array (`a(ssssssouso)`) round-trips intact.
- **Prereq discovered while building:** the system-bus bridge (`schema-systemd1.py`) had to be **restarted** — the long-lived
  PID from 2026-06-12 was wedged after days of failed polls and served 0 units even once `schema-ctl` was fixed; a fresh
  process serves a stable 38. So the relay is only as good as a healthy system-bus bridge underneath it. (See the
  degraded-state-signal hardening note — still wanted.)

### Known limitations / TODO
- Forwarding is **blocking** (`send_message_with_reply_and_block`) — fine for a low-traffic GUI, but a slow/polkit-gated
  write stalls the relay's loop until `FWD_TIMEOUT` (25s). Make async if it ever matters.
- No clean exit on session-bus disconnect (logout) → the relay may orphan until the session scope is torn down. Add a
  disconnect→quit handler.
- ~~Manager boot-timestamp properties missing~~ — DONE (master `ef7d3fd`): bridge now serves Firmware/Loader/Kernel/InitRD/
  Userspace/Finish timestamps (realtime+monotonic) + `Environment`. schema-init has no firmware/loader handoff timing so those
  read 0 (kernel monotonic base 0, userspace ~= PID1 start), so the "Startup finished in …" readout shows ~0 but no longer
  errors. This unblocked Ferrix's System Manager / Misc / Environment tabs (they raised `UnknownProperty` on the missing
  props). Future nicety: a real `FinishTimestamp` (boot-complete moment) for an accurate userspace boot duration.

## Related

- System-bus bridge: `2026-06-20-schema-systemd1-dbus-design.md`, `scripts/schema-systemd1.py`, `services/schema-systemd1.svc`.
- Control-socket read fix that unblocked the system-bus surface: master `710cbf3` (`schema-ctl.c` heap read buffer).
