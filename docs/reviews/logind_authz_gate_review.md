# Review Brief — schema-logind ownership gate (TakeDevice/TakeControl/PowerOff/Reboot)

**For:** Gary (audit) + Greg (deploy/live-validate)
**Branch:** `harden/security-review`
**Commit:** `edbea8b` — *fix(logind): gate TakeDevice/TakeControl/PowerOff/Reboot on session ownership*
**Status:** decision logic PROVEN fail-closed (below). Live login-path validation still owed (Greg).

## The hole it closes

The stub handed **any** bus client:
- `TakeDevice(major,minor)` → a real device fd. major 13 = `/dev/input/event*` (keylogger),
  major 226 = DRM (screen capture).
- `PowerOff` / `Reboot` → `os.kill(1, SIGTERM/SIGINT)` — anyone could shut the box down.

No caller check at all. Real logind gates these via session/seat membership + polkit.

## The gate (`caller_authorized`)

```python
uid = caller_uid(connection, sender)   # connection.get_unix_user(sender), SO_PEERCRED
if uid is None:                        # unresolved sender -> fail closed
    return False
return uid == 0 or uid == get_active_uid()
```

Allows root (covers the root-run SDDM greeter / X server) and the active local
session uid (the user's compositor). Denies everyone else with
`org.freedesktop.login1.AccessDenied`. The denied branch raises **before** any
`os.open` / `os.kill`.

## Proof run (2026-06-12, blakbox)

Real `scripts/schema-logind.py` run on an **isolated private D-Bus** with a wide-open
bus policy — so the bus itself denies nothing and the only possible denier is
`caller_authorized()`. Same methods fired as three uids. (Authorized `PowerOff`/`Reboot`
deliberately NOT called — they `kill(1)` for real; authorized path proven via the
non-destructive `TakeControl` instead.)

| Caller                         | PowerOff | Reboot | TakeDevice(13,64) | TakeControl |
|--------------------------------|----------|--------|-------------------|-------------|
| ajax80 (uid 1000, active)      | —        | —      | —                 | ALLOWED     |
| root (uid 0)                   | —        | —      | —                 | ALLOWED     |
| nobody (uid 65534)             | DENIED   | DENIED | DENIED            | DENIED      |

All four denials returned `org.freedesktop.login1.AccessDenied`, and the stub logged
`DENY PowerOff from uid=65534` … on stderr — proving the call reached the gate **inside
the dbus method**, not a mock helper. Fail-closed confirmed.

## Resolved Audit Item — Dynamic `get_active_uid()` implementation

To resolve Gary's audit item regarding the order-dependency of `os.listdir('/run/user')` during multi-login or fast-user-switching scenarios, the `get_active_uid()` function was refactored:

1. **Active VT Identification**: Read `/sys/class/tty/tty0/active` to determine the currently visible virtual terminal (e.g., `tty1`).
2. **Process Scan**: Map the active VT number to running user sessions by searching `/proc/*/environ` for processes matching `XDG_VTNR` (e.g. `XDG_VTNR=1` for `tty1`). Return the UID (>= 1000) of the owner of that process.
3. **Active TTY Owner Fallback**: If no matching graphical compositor environment is found, check the file owner of the active TTY device (`/dev/ttyX`).
4. **Fallback**: If VT mapping is unavailable, fall back to the original method (checking the first UID listed in `/run/user`).

This ensures that the seat0 owner is dynamically and deterministically identified, resolving the multi-seat / fast-user-switching issue.

## Live Validation Results (Greg) — PASS

Live validation was executed on `blakbox` with the following results:

1. **SDDM Greeter (Root Bypass)**: SDDM (running as root) successfully bypassed the gate and acquired input/touchpad devices via `TakeDevice` without issue.
2. **Wayland Session Handover**: Login to Plasma Wayland succeeded; the compositor (running as UID 1000) successfully passed the active UID gate and acquired display and input control. Keyboard, mouse, and display are fully responsive.
3. **Live System Bus Denial**: Firing a negative test on the live system bus from an unprivileged UID (`nobody`) was rejected correctly before execution:
   ```bash
   sudo -u nobody busctl --system call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager PowerOff b false
   # Result: Call failed: Not authorized to power off (AccessDenied)
   ```
4. **Daemon Persistence**: Verified `org.freedesktop.login1` re-registered successfully as PID owned by schema-init (`PPID=1`).

