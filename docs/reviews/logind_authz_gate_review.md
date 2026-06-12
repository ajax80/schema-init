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

## Audit item for Gary — `get_active_uid()` is order-dependent (robustness, not a hole)

```python
uids = [int(d) for d in os.listdir('/run/user') if d.isdigit() and int(d) >= 1000]
if uids:
    return uids[0]      # FIRST in arbitrary os.listdir() order
```

- Single-user box (all our boxes): correct.
- **Two simultaneous logins** (uid 1000 + 1001): trusts whichever `listdir` yields first,
  and *denies the other legitimate active user* — non-deterministic.
- **Stale `/run/user/<uid>`** left by a logged-out user: could be trusted though that user
  has no live session.

Fail direction is always *too restrictive* or *trusts a real (if stale) uid* — it never
grants an attacker who lacks a `/run/user` entry ≥1000. So not an escalation, but it
should enumerate actual seat sessions (seat0 active session uid) rather than trust
`listdir` order before multi-seat / fast-user-switching boxes rely on it.

## Still owed (Greg) — live login-path validation

The proof covers the *decision*. It does NOT prove the real greeter + compositor still
acquire input + DRM through the gate. Per the commit message: deploy canonical
`schema-logind.py`, then on one box (blakbox) confirm:
1. SDDM greeter still gets its input devices (can type the password).
2. The Wayland session comes up with working keyboard/mouse + display (TakeDevice as root
   greeter and as the session uid both succeed live).
3. A non-owner is denied on the **live system bus** (e.g. `sudo -u nobody busctl --system
   call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager
   PowerOff b false` → `AccessDenied`, box stays up).
