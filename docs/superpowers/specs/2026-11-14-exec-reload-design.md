# exec_reload= / reload_signal= / `schema-ctl reload <svc>` (audit 4.1)

## Problem
`systemctl reload X` (shim) becomes `schema-ctl restart X`: dropped
connections, lost state. `ReloadUnit` over D-Bus SIGHUPs only when the .svc
says `reload=1`, and does it from Python, outside PID 1. The importer drops
`ExecReload=`.

## Keys
- `exec_reload=CMD` — repeatable (up to 8, like `exec_stop`), same `-`
  prefix rule. `$MAINPID` / `${MAINPID}` in a word are replaced by the
  supervised pid; `MAINPID=` is also in the runner's environment.
- `reload_signal=SIG` — `HUP`, `SIGHUP`, `USR1`, `URG`, a number 1–64…
  Sent to the supervised pid. Bad values are refused at parse (#514 rule).
- `reload=1` (existing, read by the systemd1 shim) now means
  `reload_signal=HUP` when no `reload_signal=` is given.
- Both set: `exec_reload` wins (systemd: ExecReload= wins over a
  notify-reload's ReloadSignal=).

## `schema-ctl reload <svc>`
Bare `reload` and `reload --evict` keep meaning "re-read the .svc files".
Any other argument names a service:
- not found → `err: not found: X`
- not running → `err: X is not running`
- no method → `err: cannot-reload: X has no exec_reload= or reload_signal=`
  (the shim keys its restart fallback on `cannot-reload`)
- a reload runner already going → `err: X is already reloading`
- signal → `ok: SIGHUP → X (pid N)`
- commands → `ok: exec_reload → X (pid N)`; one runner child, own process
  group, as the service's user (same drop as the stop runner), reaped by
  `reap()`, killed after `stop_timeout_sec` (default grace) with
  `reload-timeout` logged. Failure logs `reload-fail`. PID 1 never waits:
  the reply is sent once the runner is spawned.
- `reload_pid`/`reload_start` are runtime fields: they ride a config reload
  and a re-exec (SVC_RUNTIME_FIELDS); eviction tracks a running one.
- A reload never touches state, restart counts or the recovery arc.

## Callers
- systemctl shim: `reload`, `reload-or-restart`, `try-reload-or-restart`
  send `reload <svc>` for an active service; if PID 1 answers "cannot
  reload", it restarts instead (today's behaviour, so a .svc imported before
  this keeps working). `reload` of an inactive service stays a no-op.
- systemd1 shim: `CanReload` = any of the three keys. `ReloadUnit` →
  `schema-ctl reload`. `ReloadOrRestartUnit`/`ReloadOrTryRestartUnit`
  reload when it can, else restart.
- Importer: every `ExecReload=` line of the form `kill [-s] -SIG $MAINPID`
  (any kill path) → `reload_signal=SIG`; anything else → `exec_reload=`
  lines, `$MAINPID` kept for the engine. `Type=notify-reload` →
  `reload_signal=` from `ReloadSignal=` (default HUP). `+`/`!` prefixes:
  dropped with a warning, as for exec_stop (the runner drops to the user).

## Not in scope
Waiting for the reload to finish (systemd blocks `systemctl reload` until
ExecReload= exits); `RELOADING=1` notify handshake.
