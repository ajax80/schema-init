# schema-dbus service activation — design

**Date:** 2026-09-04
**Status:** approved, pre-implementation
**Depends on:** 2026-09-02-schema-dbus-sp1-broker-design.md (SP1 broker, live), schema-dbus v1.1 (six-bug fixes, deployed)

## Problem

The SP1 broker has no bus activation: `org.freedesktop.DBus.StartServiceByName`
is a stub returning `ServiceUnknown`, and a method call to an unowned but
activatable name errors instead of spawning the service. Stock dbus-daemon does
both. Under the broker, every D-Bus service that was previously started
on-demand goes dark unless the schema-init rail already launches its daemon
eagerly.

Live audit (`scripts/sdbus-activatable-audit.sh`, 67 activatable services):

- **(a) 11 railed** — already started by the schema-init rail (wpa_supplicant1,
  Accounts, Avahi, PolicyKit1, UDisks2, UPower, + the shims). Covered today.
- **(b1) 43 regression set** — real `Exec=`, direct-activated on demand by stock
  dbus-daemon, dark under the broker. Overwhelmingly KDE kauth/polkit helpers
  (short-lived, spawned per privileged GUI action) plus fwupd, PackageKit,
  ColorManager, GeoClue2, bolt, fprintd, RealtimeKit1, ModemManager1, lvmdbus1.
- **(b2) 13 not a regression** — `Exec=/bin/false` + `SystemdService=`, already
  dark pre-broker (no systemd as PID 1). Out of scope.

This is not a per-service fix; it is a general mechanism the broker lacks. Build
the mechanism and all 43 in (b1) are covered.

## Decisions (locked in brainstorming)

1. **Trigger scope: full auto-activation.** Any method call to an unowned-but-
   activatable name triggers spawn + holds the triggering message + delivers it
   once the name is claimed. Also handles explicit `StartServiceByName`. The
   kauth helpers are reached by implicit calls, not explicit starts — implicit
   is what actually covers (b1).
2. **Spawn model: broker forks + execs directly.** Traditional dbus-daemon
   model. Self-contained in the broker; PID 1 stays untouched. On-demand helpers
   are not boot-critical, so the rail's restart/cgroup/watchdog guarantees do not
   apply to them anyway. (schema-init's control socket only starts services
   already declared in the rail; there is no "spawn arbitrary Exec= as User="
   verb, so delegating would mean changing PID 1 — rejected.)
3. **Security: match stock, always sanitize.** Any bus client can trigger
   activation (the subsequent method call is still policy-gated, exactly as stock
   dbus-daemon). Always honor `User=` from the file, always `setuid` down,
   sanitize env, rely on CLOEXEC for fd hygiene. No per-caller activation policy
   gate (would diverge from the parity goal and risk silently breaking flows).
   No audit log.

## Architecture

New header `sdbus_activate.h` owns the tables and the pure table logic; the
main loop in `schema-dbus.c` owns fork/exec, the epoll signalfd, and the
deadline integration (main-loop concerns).

### Data model (`sdbus_activate.h`)

**Service table** — parsed once at startup:

```
typedef struct { char *name; char **argv; char *user; } sdbus_svc_ent;
```

One entry per `/usr/share/dbus-1/system-services/*.service` whose `Exec=` is a
real binary. Entries with `Exec=/bin/false` are skipped (stay non-activatable →
keep erroring `ServiceUnknown`, matching today).

**Pending-activation table** — one entry per name currently being brought up:

```
typedef enum { SDBUS_HELD_IMPLICIT, SDBUS_HELD_EXPLICIT } sdbus_held_kind;
typedef struct {
    unsigned char *bytes; int len;   /* captured wire message (implicit only) */
    int *fds; int nfds;              /* its passed fds (implicit only) */
    int caller_id; uint32_t serial;  /* who to answer, and their serial */
    int expects_reply;
    sdbus_held_kind kind;
} sdbus_held_msg;

typedef struct {
    char *name; pid_t child_pid; long spawn_deadline_ms;
    sdbus_held_msg *held; int n_held;
} sdbus_pending_act;
```

### Parse (startup)

Glob the system-services dir; line-parse `Name=` / `Exec=` / `User=` (same
style as the audit script). `Exec=` is split into argv on whitespace. `User=`
defaults to `root`, resolved via `getpwnam` at spawn time (not parse time).
Malformed and `Exec=/bin/false` entries dropped. Fixed in-memory table; no
reload for v1 (SIGHUP reload is existing backlog, out of scope).

### Trigger + hold — the core hook

Both entry points funnel to `activate_or_hold(name, captured_request)`:

- **Implicit** — in `handle_message`, the `synth` branch (currently
  `schema-dbus.c:249-250`, "name has no owner"). Before erroring: if
  `msg->destination` is in the service table, capture the message (raw bytes +
  its fds + `caller_id` + `serial` + `expects_reply`), append it to the pending
  entry for that name, and — if no entry existed yet — spawn. The message is
  **held**, not answered. If the destination is not activatable, fall through to
  the existing `ServiceUnknown` error (unchanged).
- **Explicit** — `StartServiceByName` special-cased in `handle_message`'s
  `to_driver` path, *before* `sdbus_driver_dispatch` (it needs fork/epoll
  concerns the driver module cannot reach). Same hold, `kind=EXPLICIT`, no
  captured body.

Races: if the name is *already owned* when we reach the hook (claimed between
route decision and here) → implicit re-routes normally (fresh
`sdbus_route_targets`); explicit replies `ALREADY_RUNNING` (2). If an entry is
already `SPAWNING` → append to it, no second fork.

### Spawn mechanics + security

`fork()`. In the child:

1. `setsid()`.
2. Resolve `User=` via `getpwnam` → `initgroups()` + `setgid()` + `setuid()`
   (privileges dropped before exec).
3. Build a clean environment: `PATH`, `DBUS_STARTER_ADDRESS=<bus socket>`,
   `DBUS_STARTER_BUS_TYPE=system` (stock parity).
4. `execv(argv[0], argv)`.

All broker fds (listen, epoll, per-conn, signalfd) are `CLOEXEC`, so exec closes
them — no descriptor leak into the activated service. On `execv` failure the
child writes nothing and `_exit(127)`; the parent learns via SIGCHLD (see
Failure paths).

Parent records `child_pid` and `spawn_deadline_ms = now + 25000` (stock's
timeout).

### Release (success)

Hook into the existing name-acquisition path. `broadcast_transitions`
(`schema-dbus.c:145`) already fires when a name gets `new_owner >= 0`. Add: on a
newly-acquired name, look up the pending table; if a pending entry exists, flush
every held message and clear the entry:

- **IMPLICIT** — re-inject exactly as a fresh send: re-run `sdbus_route_targets`
  now that an owner exists (records the pending-reply entry normally so the
  eventual reply routes back), reforward the captured bytes + fds to the new
  owner.
- **EXPLICIT** — synthesize a `StartServiceByName` method_return carrying
  `DBUS_START_REPLY_SUCCESS` (1) to the caller.

### Failure + timeout paths

- **Timeout** — fold a second deadline source into the `epoll_wait` timeout
  computation (alongside `sdbus_replies_next_deadline`, `schema-dbus.c:427`). On
  expiry: reply `org.freedesktop.DBus.Error.TimedOut` to every held caller,
  drop the entry. Do not kill the child (it may still be coming up or will die
  on its own; it will be reaped by SIGCHLD).
- **Spawn failure** (fork fails) — immediate
  `org.freedesktop.DBus.Error.Spawn.Failed` to held callers, no entry created.
- **Child died before claiming its name** (includes exec failure → `_exit(127)`)
  — `org.freedesktop.DBus.Error.Spawn.ChildExited`, via SIGCHLD below.

### SIGCHLD / child lifecycle

The broker has no signal handling today. Add a `signalfd` for `SIGCHLD`
(`SFD_CLOEXEC`, with `SIGCHLD` blocked via `sigprocmask`), registered in epoll
with a dedicated sentinel `data.ptr`. On its event: drain the signalfd, then
`waitpid(-1, WNOHANG)` in a loop reaping all exited children (no zombies). For
each reaped pid: if it maps to a **still-pending** activation (name never
claimed), fail that entry's held messages with `Spawn.ChildExited` and drop the
entry. A child that exits *after* claiming its name is normal (short-lived
kauth helpers) — its entry is already cleared, so it is simply reaped.

## Blast radius

- **New:** `sdbus_activate.h`, `tests/test_sdbus_activate.c`.
- **Modified `schema-dbus.c`:** ~4 hook points — the `synth` branch (implicit
  trigger), the `to_driver` path (explicit `StartServiceByName`),
  `broadcast_transitions` (release on name-acquire), and `main` (signalfd setup
  + second deadline in the `epoll_wait` computation + the SIGCHLD reap block).
- **Unchanged:** policy, names, route, reply, match, conn, auth, wire, codec
  modules. PID 1 (`init.c`) untouched.

## Testing

- **Unit** (`tests/test_sdbus_activate.c`, added to the existing `make test`
  harness): service-file parse (real `Exec=` vs `/bin/false` skip, `User=`
  parse, argv split), hold/release table logic (implicit + explicit), timeout
  expiry produces `TimedOut`, child-died-while-pending produces
  `Spawn.ChildExited`, already-owned race → re-route / `ALREADY_RUNNING`. Spawn
  path exercised with a stub helper binary that claims a name; table/hold logic
  needs no root.
- **Live**: pick a real (b1) service (e.g. `fwupd` or a kauth helper), call it
  cold, confirm it spawns and answers instead of `ServiceUnknown`. Re-run the
  audit to confirm the regression set is now covered by mechanism.
- **vmtest** (`schema-vmtest`) before any hardware reboot — mandatory. Then the
  standard deploy path (`make schema-dbus` → back up `/usr/local/bin/schema-dbus`
  → `sudo make install-dbus-sp1` → reboot), rollback binary preserved.

## Out of scope

- Session-bus activation (SP4 / "SP2" — separate subproject).
- Service-file reload on SIGHUP (existing v1.1 backlog).
- Delegating spawn to PID 1 (rejected — decision 2).
- `SystemdService=`-only entries (b2 set) — cannot spawn, stay `ServiceUnknown`.
