# schema-dbus SP4 — session bus reclamation — design

**Date:** 2026-09-22
**Status:** approved, pre-implementation
**Depends on:** 2026-09-02-schema-dbus-sp1-broker-design.md (broker core, live on system bus since 2026-09-03), 2026-09-04-schema-dbus-activation-design.md (v1.1 activation, live)

## Problem

The system bus is fully reclaimed (schema-dbus C broker owns
`/run/dbus/system_bus_socket`, live since 2026-09-03). The session bus —
one per login, unprivileged, carrying the bulk of desktop chatter
(portals, notifications, kauth helpers, app activation) — is still stock
`dbus-daemon`, spawned per-login via `plasma-dbus-run-session-if-needed`
(the classic `dbus-launch` model; schema-init runs no `systemd --user`).
This is the last piece of the desktop D-Bus stack not owned by schema-init.

Goal (Jonathan's call, 2026-09-22): **reclamation completionism** —
schema-dbus owns the session bus, stable, no regressions. Not chasing a
specific perf/bug complaint; the earlier "desktop feels fast" measurement
question stays parked (absolute numbers can still be captured for free,
per [[project_schema_dbus]]'s existing README-numbers precedent, but no
new A/B harness work is in scope here).

## Why this is smaller than the system-bus flip

The system-bus effort (SP0–SP3) spent most of its time on a felt-policy
learner + 18h/83k-message corpus + fidelity gate, because the system bus's
`/usr/share/dbus-1/system.conf` encodes 209 real access-control contexts
that had to be reproduced exactly.

The session bus has no equivalent problem. Its actual policy
(`/usr/share/dbus-1/session.conf`, verified live 2026-09-22) is:

```xml
<policy context="default">
  <allow send_destination="*" eavesdrop="true"/>
  <allow eavesdrop="true"/>
  <allow own="*"/>
</policy>
```

Allow everything, to anyone. The only local override
(`session.d/snapd.session-services.conf`) just adds a service directory,
no restrictions. There is nothing to learn — **the SP0 learner/corpus/
fidelity-gate phase is skipped entirely.**

The broker's existing default policy when no policy file is supplied
(`schema-dbus.c`: `"context = default\nallow = send_destination:*\n"`)
already matches this. No policy engine needs to be loaded for the session
bus at all.

Risk class is also lower than the system-bus flip, not equal to it: the
system bus is PID-1-rail-managed, so swapping it required a reboot and
the boot-rollback guard. The session bus is spawned post-login by
`plasma-session-start.sh` — cutover is a logout/relogin (or restarting
the autologin service), fully SSH-recoverable, no GRUB/boot-guard
involvement.

## Decisions (locked in brainstorming)

1. **Reuse the existing `schema-dbus` binary as-is — no new build, no new
   mode.** It is already environment-variable-driven
   (`SCHEMA_DBUS_SOCKET`, `SCHEMA_DBUS_POLICY`, `SCHEMA_DBUS_SVCDIR`); the
   `--system` flag today only affects a log line, not behavior. Session
   use is a matter of what env vars + servicedir a launcher script passes
   in, plus two real bug fixes (below) — not a new binary mode.
2. **No policy engine load for the session bus.** Rely on the broker's
   existing no-policy-file default (`allow send_destination:*`), which
   already matches `session.conf`'s stance. Do not synthesize or dissolve
   a session policy file.
3. **The `schema-systemd1-session` shim stays a separate process,
   unmerged.** It already runs today as an independent client on
   whatever bus backs the session (proven 2026-09-22: `StartUnit`/
   `StartTransientUnit` calls flow through it regardless of broker
   identity). This matches the existing system-bus precedent — the
   system-bus `schema-systemd1` shim is also a separate rail service, not
   folded into `schema-dbus`. No reason to change that shape here.
4. **Rollback: baked-in self-heal**, matching the pattern
   `schema-dbus-run.sh` already uses for the system bus. The new
   session-bus launcher tries `schema-dbus`; on failure to bind/start it
   falls back to stock `dbus-daemon` automatically. A broken flip never
   costs a locked-out relogin.
5. **Bus address: adopt the XDG standard `$XDG_RUNTIME_DIR/bus`**,
   replacing the current `dbus-launch`-style random `/tmp/dbus-XXXXXXXX`
   socket. This is the modern convention several tools probe for
   directly even without `DBUS_SESSION_BUS_ADDRESS` set; no reason to
   keep perpetuating the legacy pattern now that schema-init controls the
   launch script directly.
6. **Out of scope:** felt-policy learner/corpus/fidelity-gate (nothing to
   learn — see above), merging the systemd1 shim into the broker,
   multi-user/multi-session support (blakbox is single-user), a new
   dbus-probe A/B measurement harness for the session bus (can be added
   later, trivially, by pointing the existing `dbus-probe.sh` at the new
   socket — not blocking this work).

## Two required code fixes

Both in the existing broker source, both small, both unit-testable in
isolation before any live session is touched.

### Fix 1 — activation default user must not be `"root"` in session mode

`sdbus_activate.h`, `sdbus__svc_add`-adjacent parse path: when a
`.service` file carries no `User=` key, the table currently defaults to
`"root"` (`e->user = strdup(user && *user ? user : "root")`). This is
correct for the system bus (stock daemon-set services nearly always
specify `User=` explicitly, or are genuinely meant to run as root when
they don't). It is wrong for the session bus: session `.service` files
essentially never carry `User=` because under real `systemd --user`
that's implicit "run as the session owner." Left unpatched, every
session-activated app (the KDE portals, kauth-adjacent session helpers,
anything landing through `sdbus_activate.h`'s spawn path) would launch as
root the instant this broker owns the session bus — a real privilege
escalation, not a cosmetic bug.

**Fix:** thread a "default user" through the parse call (env var or
param, e.g. `SCHEMA_DBUS_DEFAULT_USER`, unset → today's `"root"`
behavior unchanged for the system bus; session launcher sets it to the
invoking user). `spawn_service`'s existing fail-closed behavior on an
unresolvable `User=` (`if (!pw) _exit(127)`) is correct and stays as-is
— this fix only changes what "absent" resolves to, not the fail-closed
path for a genuinely bad explicit `User=`.

### Fix 2 — activation table needs a directory search list, not one dir

`sdbus_svctab_parse_dir_masked(dir, maskfile)` scans exactly one
directory. The system bus only ever needed one
(`/usr/share/dbus-1/system-services`). The session bus needs at least
two, in override order (first match wins, matching
`standard_session_servicedirs` semantics):

1. `~/.local/share/dbus-1/services` — user-level overrides (flatpak
   exports, the systemd1-shim's own `.service` registration, any future
   user-scoped services)
2. `/usr/share/dbus-1/services` — system-wide session service files
   (the KDE portals, snapd's servicedir-extension pattern)

**Fix:** extend to `sdbus_svctab_parse_dirs_masked(const char **dirs, int
ndirs, const char *maskfile)`, iterating dirs in order and skipping a
`Name=` already present in the table (first-wins = override semantics).
Keep `sdbus_svctab_parse_dir_masked` as a one-dir wrapper around it so
the system-bus call site (`SCHEMA_DBUS_SVCDIR`, singular, unchanged) is
untouched. New env var `SCHEMA_DBUS_SVCDIRS` (plural, colon-separated,
same convention as `PATH`) selects the multi-dir path in `main()`; when
unset, behavior is identical to today.

## Launcher: `schema-dbus-session-run.sh`

New script, sibling to the existing `scripts/schema-dbus-run.sh` (which
dissolves the live system policy and execs the broker for the system
bus). This one is simpler — no policy dissolution step:

```
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
export SCHEMA_DBUS_SOCKET="$XDG_RUNTIME_DIR/bus"
export SCHEMA_DBUS_DEFAULT_USER="$(id -un)"
export SCHEMA_DBUS_SVCDIRS="$HOME/.local/share/dbus-1/services:/usr/share/dbus-1/services"
# unset SCHEMA_DBUS_POLICY -> broker's no-policy-file default (allow-all) applies
exec_schema_dbus_or_fallback_to_stock_dbus_daemon   # self-heal, see Decision 4
```

Wired into `plasma-session-start.sh` in place of the current
`plasma-dbus-run-session-if-needed` invocation. Self-heal: if
`schema-dbus` fails to bind the socket or exits immediately, fall back to
`dbus-daemon --session --address="unix:path=$XDG_RUNTIME_DIR/bus"
--fork` — same shape as `schema-dbus-run.sh`'s existing self-heal for the
system bus, so the session always gets *a* working bus even on a broken
flip.

## Testing plan

1. **Unit tests** for both fixes (default-user threading, multi-dir
   search/override) — extend the existing `sdbus_activate` test suite,
   `make test` green.
2. **Isolated scratch-bus proof**, before touching the live login — same
   `dbus-run-session`/userns pattern already used for the system-bus
   shim-check (`tests/sdbus_shim_check.sh`) and the session-shim
   `StartUnit` tests. Start `schema-dbus` on a throwaway socket with real
   session servicedirs, then by hand:
   - connect a real client, `RequestName`/`ReleaseName`, `ListNames`
   - trigger activation of a real portal `.service` by name, confirm it
     spawns as the *invoking* user (not root) — this is the direct
     regression test for Fix 1
   - confirm a service in `~/.local/share/dbus-1/services` shadows a
     same-named one in `/usr/share/dbus-1/services` — direct test for
     Fix 2
   - client-to-client method call + reply routing (already proven
     broker-generic code, re-confirm it still holds under session env)
3. Only after (1)+(2) pass does the launcher go into
   `plasma-session-start.sh` for a real cutover.

## Cutover

On blakbox, Jonathan present (not unattended):
1. Back up current session-start invocation (the
   `plasma-dbus-run-session-if-needed` line) in
   `plasma-session-start.sh`.
2. Swap in `schema-dbus-session-run.sh`.
3. Log out / log back in (or restart the autologin service) — no reboot.
4. Verify: `busctl --user list` (or equivalent) shows the broker as
   bus owner; portals registered (`pgrep -af xdg-desktop-portal`); no
   new stalls; `schema-systemd1-session` still functioning as a client
   (spawn-on-open still works, per the 2026-09-22 StartUnit
   verification).
5. **Rollback**, if needed: revert the `plasma-session-start.sh` line
   and relogin — no boot-guard, no reboot, fully SSH-recoverable from
   another host if the live session itself is unusable. Self-heal
   (Decision 4) should mean this manual path is rarely needed.

## Out of scope (restated)

Felt-policy learner/corpus/fidelity gate, merging the systemd1 shim into
the broker, multi-user support, a dedicated session-bus A/B measurement
harness.
