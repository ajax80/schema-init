# schema-logind: gate the unauthenticated mutators

## Problem

Same class as the #469 systemd1 bridge hole. `schema-logind.py` runs as root and
serves hostname1/timedate1/locale1 (whose stock bus policy is `allow
send_destination` for everyone, because real systemd checks polkit inside the
daemon) plus login1 session methods. Only a handful of methods
(TakeControl/TakeDevice/SwitchTo/SetUserLinger/PowerOff/Reboot/Restart/Stop)
were gated; the rest ran with no caller check.

Confirmed live on DBox (2026-11-04), as `sudo -u nobody busctl
--allow-interactive-authorization=no`, benign no-op values: SetStaticHostname,
SetTimezone, SetLocale, SetVConsoleKeyboard all accepted; UnlockSession accepted.
An independent audit (Greg) flagged the hostname1/timedate1/locale1 gap as its
top finding.

Not code execution: input validation is already solid (locale rejects `\r\n\0`
and unknown vars, keyboard setters reject quotes/newlines, hostnames are
char-restricted, timezones are path-checked). The impact is config spoofing,
screen-lock bypass, and device-fd DoS — all local, by any uid.

## Fix

`authz_check(connection, sender, what)` reuses the existing `caller_authorized`
model (root, or the active session's own user; unresolved sender fails closed)
and raises `org.freedesktop.DBus.Error.AccessDenied` before any side effect. It
is the first statement of every gated method.

Gated now: hostname1 Set{Hostname,StaticHostname,PrettyHostname,IconName,
Chassis,Deployment,Location}; timedate1 SetTimezone; locale1
Set{Locale,VConsoleKeyboard,X11Keyboard}; login1 Session.{Unlock,Activate,
ReleaseControl,ReleaseDevice,PauseDeviceComplete}, Manager.{UnlockSession,
UnlockSessions,ReleaseSession}. (Activate/ReleaseControl/PauseDeviceComplete/
ReleaseSession added after code review: all reachable VT/session-state mutators
of the same DoS class; the active session's own compositor is the active uid, so
caller_authorized still lets it through.)

Left open deliberately: Get*/List*/Can* (reads); SetTime/SetNTP/SetLocalRTC,
ScheduleShutdown, SetWallMessage, Manager.ActivateSession, Terminate* (no-op
`pass`/print bodies here); Lock/LockSession (locking is not a threat);
SetIdleHint/SetLockedHint (the compositor/locker reports its own state); Inhibit
(returns a throwaway fd, no real inhibitor state); RearmVtMediation (custom
interface, not in the stock bus policy, so not reachable by non-root).

## Not in this change

- **CreateSession** writes a caller-named pid into a cgroup as root. The login1
  bus policy already denies it to non-root (verified), so it is not reachable;
  an in-daemon backstop is a possible follow-up, but a flat root check breaks
  the non-root logic test and the right invariant ("caller as privileged as the
  daemon") needs its own design.
- **get_active_uid() multi-user pick** (`min(uids)`): already documented as a
  known follow-up in `docs/reviews/logind_authz_gate_review.md`; a single-seat
  box is unaffected.

## Verified

`tests/test_logind_authz.py`: the gate decisions (root/active/stranger/no-sender)
and that all 15 targeted methods consult the gate before their body. `make test`
and the Python suite pass. Live on DBox: nobody denied for all of the above,
root and the active desktop user allowed, PowerOff still denied, logind HUP kept
its pid and the session.
