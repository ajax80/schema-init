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

1. **Reuse the existing `schema-dbus` binary as-is — no new build.** It is
   already environment-variable-driven (`SCHEMA_DBUS_SOCKET`,
   `SCHEMA_DBUS_POLICY`, `SCHEMA_DBUS_SVCDIR`). The `--system` flag today
   only affects a log line (`system_bus` is parsed but otherwise unused)
   — this design gives it real meaning: **`--system` present vs absent
   becomes the single mode signal** gating all four fixes below (private
   `g_system_bus`, set once in `main()`). The session launcher simply
   omits the flag, exactly as it omits `SCHEMA_DBUS_POLICY`. No separate
   new mode env var.
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

## Required code fixes

All in the existing broker source, all gated on the `g_system_bus` mode
signal (Decision 1) so system-bus behavior is byte-for-byte unchanged,
all unit-testable in isolation before any live session is touched.
Found via external review (Greg/Gemini, 2026-09-22) against this spec's
first draft and verified line-by-line against the real source + the
repo's own test suite before being folded in here — the four fixes
below. One suggested addition (`allow = own:*`, folded into Fix 3) was
checked and found not load-bearing (RequestName never
consults the policy engine at all — grepped every `sdbus_policy_eval`
call site; the only one hardcodes `req.op = "send"` — so `own`/
`own_prefix` predicates exist in the matcher but are dead code today).
Included anyway since it costs nothing and documents intent, but it
does not fix an active bug the way the others do.

### Fix 1 — `spawn_service` crashes every activation in session mode

`schema-dbus.c:564-586`. Two separate bugs in the same function, both
real, both would make session activation DOA (not just insecure) if
shipped as originally drafted:

**1a — privilege-drop syscalls are unconditional.** `initgroups()`/
`setgid()`/`setuid()` run whenever `pw->pw_uid != 0`, with no check for
whether the broker itself is already running as that uid. In session
mode the broker runs as the login uid (e.g. 1000); `setgroups(2)`
(underlying `initgroups()`) requires `CAP_SETGID` regardless of whether
the target set matches the caller's current groups — so it fails EPERM
and `_exit(127)`s on literally every activation, even one that resolves
correctly to the session's own user. **Fix:** skip the
initgroups/setgid/setuid block entirely when `!g_system_bus` (the broker
is already running as the target user by construction — there is no
lower-privilege user to drop to, matching how a real `systemd --user`
manager never drops privilege either).

**1b — the "absent `User=`" default is hardcoded `"root"`.** Session
`.service` files essentially never carry `User=` (under real `systemd
--user` that's implicit "run as the session owner"). Correct for the
system bus (kept as-is); wrong for the session bus, where it would still
matter for any explicit non-matching `User=` value. **Fix:** in
`sdbus_activate.h`'s parse path, thread a default-user string computed
once in `main()`: `g_system_bus ? "root" : getpwuid(getuid())->pw_name`
— no new env var, the broker already knows who it's running as.
`spawn_service`'s existing fail-closed behavior on a genuinely
unresolvable explicit `User=` (`if (!pw) _exit(127)`) is unchanged.

### Fix 2 — spawned children get a stripped env with `DBUS_STARTER_BUS_TYPE` hardcoded to `"system"`

`schema-dbus.c:578-586`. The child env array is exactly `PATH`,
`DBUS_STARTER_ADDRESS`, and a literal `DBUS_STARTER_BUS_TYPE=system` —
nothing else, unconditionally. Correct-ish for system-bus services
(stock dbus-daemon also gives system-activated services a minimal env).
Fatal for session activation: KDE portals and any session-activated app
need `WAYLAND_DISPLAY`/`DISPLAY` (compositor), `XDG_RUNTIME_DIR`,
`HOME`, `USER`, `XDG_DATA_DIRS` (resource lookup) — none of which reach
the child today. **Fix:** when `!g_system_bus`, build the child env by
copying the broker's own `environ` (`extern char **environ`) and
appending/overriding `DBUS_STARTER_ADDRESS` and
`DBUS_STARTER_BUS_TYPE=session` on top, instead of the 3-entry clean
array. System-bus behavior (`g_system_bus` true) is untouched — keep the
existing clean-env array exactly as it is; it matches stock dbus-daemon
system-bus behavior and already has real production mileage.

### Fix 3 — default fallback policy denies broadcast signals (and, harmlessly, ownership)

`schema-dbus.c:~617`, the no-policy-file fallback string:
`"context = default\nallow = send_destination:*\n"`. Verified against
`sdbus_policy.h`: the `send_destination` rule matcher returns no-match
for `n_dest_names==0` (an undirected/broadcast signal) even against
`value=="*"` (`sdbus_policy.h` ~line 202: `if (n == 0) return 0;`), and
`sdbus_policy_eval`'s default verdict is deny with no matching rule.
This isn't inferred — the repo's own `tests/test_sdbus_route.c:42-43`
already needs `allow = send_type:signal` alongside `send_destination:*`
for its own broadcast-signal test case to pass. Session-bus traffic
leans heavily on undirected signals (property-change notifications,
KSplash/portal state signals, etc.) — this fallback string has never
actually been exercised in production before now (the system-bus
launcher always supplies a dissolved policy file, so this code path is
essentially untested-by-use). **Fix:** the default fallback string
becomes:
```
context = default
allow = send_destination:*
allow = send_type:signal
allow = own:*
```
(`own:*` included per the finding above — inert today, free, documents
intent for if/when ownership ever does get policy-gated.)

### Fix 4 — activation table needs a directory search list, not one dir

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
bus). This one is simpler — no policy dissolution step, and critically,
**no `--system` flag** (that absence is what puts the broker in session
mode per Decision 1):

```
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
export SCHEMA_DBUS_SOCKET="$XDG_RUNTIME_DIR/bus"
export SCHEMA_DBUS_SVCDIRS="$HOME/.local/share/dbus-1/services:/usr/share/dbus-1/services"
export SCHEMA_DBUS_MASKFILE=/dev/null   # do NOT inherit the system bus's
                                         # /etc/schema-dbus/masked (masks
                                         # org.freedesktop.PolicyKit1, a
                                         # system-bus-only concern)
# unset SCHEMA_DBUS_POLICY -> broker's no-policy-file default applies (Fix 3)
exec "$BROKER"     # no --system flag; foreground, exec'd (see Execution model)
```

Wired into `plasma-session-start.sh` in place of the current
`plasma-dbus-run-session-if-needed` invocation, **backgrounded with `&`**
the same way that script already backgrounds `kwin_wayland`/
`plasmashell` — not self-daemonized. Self-heal: if `schema-dbus` fails to
bind the socket or exits immediately, fall back to `dbus-daemon --session
--address="unix:path=$XDG_RUNTIME_DIR/bus" --nofork` — same shape as
`schema-dbus-run.sh`'s existing self-heal for the system bus (which uses
`exec "$STOCK" --system --nofork`), so the session always gets *a*
working bus even on a broken flip.

### Execution model

`schema-dbus-run.sh` (system bus) ends with `exec env ... "$BROKER"
--system` — it replaces itself with the broker, becoming the long-running
foreground process that the schema-init rail supervises as a `.svc`
leader. The session launcher has no rail/PID-1 supervision to plug into
(`plasma-session-start.sh` is a plain shell script, not a rail unit) —
it must end the same way (`exec`, not fork/daemonize) so that the `&`
backgrounding in `plasma-session-start.sh` tracks the actual broker (or
fallback `dbus-daemon --nofork`) process directly, exactly like the
`kwin_wayland --drm --xwayland ... &` line already does.

## Testing plan

1. **Unit tests** for all four fixes — extend the existing
   `sdbus_activate`/`sdbus_policy`/route test suites, `make test` green:
   - Fix 1a: spawn under a non-root broker uid, confirm no
     initgroups/setgid/setuid attempted and the child actually execs
     (today it would `_exit(127)` before ever reaching `execve`)
   - Fix 1b: absent `User=` resolves to the broker's own uid in session
     mode, still resolves to `root` in system mode (regression guard)
   - Fix 2: spawned child's env contains `DBUS_SESSION_BUS_ADDRESS`-
     relevant passthrough vars in session mode; system mode env is
     byte-identical to today (regression guard)
   - Fix 3: a synthetic broadcast signal (`n_dest_names==0`) routes under
     the new default fallback string; fails under the old one (proves
     this is a real regression test, not a no-op)
   - Fix 4: multi-dir search, override precedence, `SCHEMA_DBUS_SVCDIR`
     (singular) unaffected
2. **Isolated scratch-bus proof**, before touching the live login — same
   `dbus-run-session`/userns pattern already used for the system-bus
   shim-check (`tests/sdbus_shim_check.sh`) and the session-shim
   `StartUnit` tests. Start `schema-dbus` (no `--system`) on a throwaway
   socket with real session servicedirs, then by hand:
   - connect a real client, `RequestName`/`ReleaseName`, `ListNames`
   - trigger activation of a real portal `.service` by name, confirm it
     spawns as the *invoking* user (not root) and can actually reach the
     compositor (Fix 1 + Fix 2's direct regression test)
   - emit an undirected broadcast signal, confirm a subscribed match
     receives it (Fix 3's direct regression test)
   - confirm a service in `~/.local/share/dbus-1/services` shadows a
     same-named one in `/usr/share/dbus-1/services` (Fix 4's direct
     regression test)
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
