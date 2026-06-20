# Design — `org.freedesktop.systemd1` D-Bus Surface (v1: services)

**Status:** approved design, pre-implementation
**Date:** 2026-06-20
**Branch:** `feat/systemd1-dbus-surface`
**Basis:** Greg's `systemd1_implementation_plan.md` + four fidelity refinements + Version decision

## Goal & North Star

Make schema-init expose a faithful `org.freedesktop.systemd1` D-Bus surface so that standard
systemd tooling — `systemctl`, Cockpit's Services page, a future Plasma KCM — manages
schema-init services **unmodified**. North star: indistinguishable from systemd except footprint.
This is the systemd-compat ("the key") surface; every GUI becomes a free consumer of it.

## Scope

**v1 (this spec):** services only. schema-init `.svc` units modeled as `.service` units, with
real live state, real per-unit objects, change signals, and working start/stop/restart/enable/disable.

**Out of scope (explicit follow-on milestones, each its own spec):**
`.target`/`.timer`/`.socket`/`.mount` unit types; dependency graph properties (Wants/After/Requires);
event-driven push (Approach 2); native C-in-PID1 surface (Approach 3); journal integration.

## Architecture (Approach 1 — polling shim, zero PID1 risk)

- **New process `scripts/schema-systemd1.py`** — owns the `org.freedesktop.systemd1` bus name and
  the Manager object at `/org/freedesktop/systemd1`. dbus-python + GLib main loop (mirrors
  `schema-logind.py` conventions).
- **Remove** `Systemd1Manager` class + its instantiation + its `request_name('org.freedesktop.systemd1')`
  from `scripts/schema-logind.py` (lines ~706–827, ~953, ~970–975) to avoid a bus-name conflict.
  logind keeps login1/hostname1/timedate1/ConsoleKit only.
- **State source:** poll `schema-ctl status --json` on a GLib timeout (~1s) and on-demand inside
  method handlers. JSON shape (verified): `{services_count, groups_count, services:[{name,pid,state,restarts}]}`.
- **Dynamic unit objects** at `/org/freedesktop/systemd1/unit/<escaped>` (e.g. `frigate.service` →
  `frigate_2eservice`), created/removed as units appear/disappear between polls.
- **Change detection:** diff successive polls → emit signals (below). No PID1 changes; daemon is
  `critical=0`, so a fault never affects boot.

## State Mapping (verified against `schema.h`)

schema-init state → (`ActiveState`, `SubState`). **pid-aware** so oneshots read correctly:

| schema-init state | pid | ActiveState | SubState |
|---|---|---|---|
| `PERFECT` (88) / `FUNDAMENTAL` (1) / `SETTLED` (7) / `FULL_TRUST` (10) | >0 | `active` | `running` |
| same success states | 0 | `active` | `exited` *(oneshot; set Service `Type=oneshot`)* |
| `NEW_PROCESS` (8) / `FRICTION` (6) / `RECOVERY` (9) | any | `activating` | `start` |
| `DORMANT` (75) | any | `activating` | `auto-restart` |
| `EXCISED` (76) | any | `failed` | `failed` |
| default / unknown | any | `inactive` | `dead` |

(Note: `EXCISED` = systemd `failed` is more faithful than `inactive` for a service the supervisor
gave a verdict on; revisit if a tool treats `failed` too harshly.)

## D-Bus Surface

### Manager (`org.freedesktop.systemd1.Manager`)
- **Methods:** `ListUnits` (real, from poll), `ListUnitsFiltered`, `GetUnit`, `LoadUnit`,
  `StartUnit`/`StopUnit`/`RestartUnit`/`ReloadUnit` (→ `schema-ctl`), `GetUnitFileState`,
  `ListUnitFiles`, `EnableUnitFiles`/`DisableUnitFiles`/`MaskUnitFiles`, `Subscribe`/`Unsubscribe`,
  `Reload`.
- **Properties:** `Version = "256"` (constant `SYSTEMD_COMPAT_VERSION`, mimic for compat),
  `Architecture`, `Features` (minimal/empty), `NNames`/`NJobs` as available.
- **Signals:** `UnitNew`/`UnitRemoved`, `JobNew`/`JobRemoved`, `Reloading`.

### Unit object (`org.freedesktop.systemd1.Unit` + `.Service`)
- **Unit props:** `Id, Names, Description, LoadState (=loaded), ActiveState, SubState,
  UnitFileState, FragmentPath (=/etc/schema-init/services/<name>.svc), ActiveEnterTimestamp`.
- **Service props:** `MainPID (←pid), ExecMainPID, Type, NRestarts (←restarts)`.
- `org.freedesktop.DBus.Properties` `Get`/`GetAll` + `PropertiesChanged` on state shift.

### Job lifecycle (fidelity fix #1 — required for blocking `systemctl`)
`systemctl start X` blocks on a `JobRemoved`. So control methods: allocate a real job object path,
emit `JobNew`, invoke `schema-ctl`, emit `JobRemoved (result="done"|"failed")` synchronously, return
the job path. `Subscribe()` must be honored or no client receives signals.

### Graceful degradation
Because we advertise `Version 256`, clients may probe features we don't implement (slices, scopes,
cgroup resource APIs). Unmapped **methods** return a proper `org.freedesktop.DBus.Error.NotSupported`
(or `UnknownMethod`); unmapped **properties** return empty/sane defaults. Mimic the version, fail honestly.

## Deployment
- `scripts/schema-systemd1.py` installed (build/install script updated).
- Service file `schema-systemd1.svc` (`dep=dbus`, `needs_root=1`, `critical=0`) in distro tree and
  `/etc/schema-init/services/`.

## Acceptance Tests (v1 "done")
1. `busctl introspect org.freedesktop.systemd1 /org/freedesktop/systemd1` lists the Manager surface.
2. `systemctl list-units --type=service` shows schema-init services with truthful Active/Sub state.
3. `systemctl status frigate` shows running + MainPID; `mount-efi` shows `active (exited)`.
4. `systemctl restart frigate` completes (does not hang) and the service actually restarts.
5. Cockpit Services page lists the services and reflects a live state change within ~1–2s.

## Risks
- **Bus-name handoff:** logind must drop the name before/independent of schema-systemd1 claiming it;
  never run both claiming `org.freedesktop.systemd1`.
- **dbus-python dynamic objects:** avoid path re-registration leaks on unit churn; unregister removed units.
- **Poll lag:** ~1s signal latency (acceptable for v1; Approach 2 removes it later).

## v1 Deployment Results (2026-06-20, live on blakbox)

Deployed live (logind restarted to release the name; seatd owns the seat so no
display disruption; logind backed up to `schema-logind.py.bak-20260620-systemd1`).
Two bugs surfaced only under the real deployment (not the isolated bus test):

1. **`schema-ctl` not on the service PATH** — schema-init's service env lacks
   `/usr/local/bin`, so `subprocess.run(['schema-ctl', ...])` failed and zero
   units were created. Fixed: call by absolute path (`SCHEMA_CTL`).
2. **`GetAll("")` returned `{}`** — `systemctl` fetches all properties via
   `GetAll("")` (empty interface); we only matched exact interface names, so
   `status` got nothing and aborted on the null `Id`. Fixed: empty interface
   returns the union of all interfaces' properties.

**Verified working via real systemctl:** `list-units` (35 services), `status
frigate` (active/running, MainPID 21102), `mount-efi` active(exited) oneshot,
non-root `systemctl restart` (exit 0), direct D-Bus `RestartUnit` (Cockpit/KCM
path) returns a job.

**Known limitation → next milestone:** `sudo systemctl <verb>` (root) prefers
systemd's private transport `/run/systemd/private`, which schema-init does not
provide, so root-CLI systemctl fails (non-root + all D-Bus GUIs work). Closing
this means serving the sd-bus private socket at `/run/systemd/private` — a
deliberate follow-on, since a malformed one makes systemctl prefer-then-fail.
