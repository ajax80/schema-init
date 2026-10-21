# Per-user supervisor (`schema-user`) — design

**Date:** 2026-10-20
**Status:** DRAFT — brainstormed with Jonathan 2026-10-20; Greg (Gemini) consulted.
**Roadmap:** `docs/roadmap.md` (PR #376) — `systemd --user` parity.
**Scope of this document:** Phase 1 in full; Phase 2 in outline (it gets its own spec once Phase 1 has landed and run).

## Intent

schema-init has no `systemd --user`. Two consequences:

1. **For other people** — the reason this is on the roadmap. Outsiders on a stock Fedora desktop can't adopt schema: GNOME builds its whole session from user units and black-screens without a user manager; rootless podman/Quadlet, Syncthing, user timers and anything enabled with `systemctl --user` silently never start.
2. **On Jonathan's own fleet** — found by the 2026-10-20 survey, not previously noticed. Enabled user units are silently dead:
   - Optiplex: `obsidian-tunnel.service` (ssh `-L 27124` to blakbox) — enabled in `default.target.wants`, never runs.
   - blakbox: `openclaw-gateway`, `wifi-densepose`, `gammastep`, `claude-sync.{path,timer}` (probably obsolete), `hermes-gateway`, `claude-brain` — none running, no warning.
   - Hand workarounds already standing in for user units: PipeWire/WirePlumber/pipewire-pulse (both `plasma-session-start.sh` and system `.svc`s), `blakbox-backup.timer` (hand-ported to a system `.svc`), Syncthing (XDG autostart), and the KDE session daemons launched one by one in `plasma-session-start.sh` (~200 lines, several black-screen incidents in its comments).

Each Fedora KDE box ships ~148 user units, ~22 enabled by default, and 32–41 session D-Bus activation files carry `SystemdService=` expecting a user manager.

## Goals

**Phase 1 — the supervisor.** One supervisor per user that reads the user's enabled unit files, starts them, restarts them, runs their timers, socket-activates them, and starts them when the session bus asks for a `SystemdService=` that maps to a user unit.

Phase 1 is done when, on the real fleet:
- `obsidian-tunnel` comes up by itself on Optiplex after a reboot.
- PipeWire and Syncthing run from their own unit files on blakbox, and their hand-made copies are removed.
- `schema-ctl --user status` lists the user's services with the same states/restart accounting as system services.
- Desktop login is unchanged and unaffected on blakbox, Optiplex and Eli (see Anti-goals).

**Phase 2 — the session.** Session targets plus the full session-bus `org.freedesktop.systemd1` API that desktop login managers use. Done when GNOME and stock `startplasma-wayland` log in **unmodified** on a test VM, and most of `plasma-session-start.sh` is deleted.

## Non-goals

- Phase 1 does not move the desktop login onto the supervisor. `plasma-session-start.sh` keeps launching kwin/plasmashell/KDE daemons.
- Phase 1 does not run units pulled in only by session targets (`graphical-session.target`, `graphical-session-pre.target`, `plasma-*.target`, `gnome-session*.target`). They are listed as skipped (reason: "phase 2 session target"). `gammastep` stays dead until Phase 2.
- Phase 1 does not replace the Python session shim's `StartTransientUnit` (app launches) — unchanged until Phase 2.
- No `.path` units in Phase 1 (skipped with a warning; affects only `claude-sync.path`).
- No template user units (`foo@.service`) in Phase 1 beyond what `schema-import` already handles for system units.
- No homed, no user slices' resource policy (MemoryMax etc. on the user tree) beyond what per-service `.svc` keys already give.

## Anti-goals

**In Phase 1, desktop login must never depend on `schema-user`.** Breaking the desktop login is the worst outcome (Jonathan, 2026-10-20). Concretely:

- schema-logind starts `schema-user` **beside** the login; nothing in the login path waits for it. If it crashes, hangs, or never starts, kwin/plasmashell come up exactly as today.
- **Kill switch:** `/etc/schema-init/user-supervisor.disabled` exists, **or** `schema.user=0` is on the kernel command line → schema-logind never starts `schema-user`. The cmdline form is the boot-menu recovery path (no rescue USB).
- **Nothing starts twice.** A unit the session script or a system `.svc` already provides is skipped, never started a second time (see *Skip list*).
- **Workarounds come out last.** A hand-made piece is removed only after its unit-file version has run cleanly through a reboot on that machine.
- **Session D-Bus activation never regresses.** If `schema-user` is down, activation falls back to today's direct `Exec=` (see *D-Bus activation*).
- **PID 1 changes are VM-tested before any hardware reboot** (schema-vmtest), then blakbox, then Optiplex/Eli — the #393/#394 rollout pattern.

## Constraints / facts established

- `init.c` (4259 lines) holds the engine as file-static functions: main `for(;;)` loop (`init.c:3619`), `reap()`, timers (`timer_arm_calendar`, `timer_arm_persistent`, `timer_rearm`, stamps in `TIMER_STAMP_DIR=/var/lib/schema-init/timers`), `tick_service`, readiness (`notify_poll`, `ready_*`), ctl socket (`ctl_init/ctl_poll/ctl_cmd`, `CTL_SOCK_PATH=/run/schema-init.sock`), shm (`SCHEMA_SHM_NAME="/schema-init"`), signalfd, reload. Interleaved with PID-1-only work: `mount_pseudo`, watchdog, modprobe, pstore, rescue, reexec, shutdown/`remount_all_ro`, PSI survival posture. `service.c` (2500) holds parsing, spawn, cgroup placement under `/sys/fs/cgroup/schema-init/<name>`, `service_listen_parse` (socket activation).
- `main()` refuses `getpid() != 1` (`init.c:3884`). Keep that guard.
- Readiness/notify identifies the sender by **cgroup** (`NOTIFY_SOCK_PATH=/run/schema-init/notify`). A non-root supervisor cannot create cgroups unless given a delegated subtree.
- schema-logind is Python (`scripts/schema-logind.py`), runs as root, creates `/run/user/<uid>` (0700, chowned) at startup, reports `KillUserProcesses=false`, `SetUserLinger` is a no-op stub (`:1832`).
- The session bus lives at `$XDG_RUNTIME_DIR/bus`, started by `schema-dbus-session-run.sh` inside the session (not before login; absent for a lingering user with no session).
- `schema-import.py` (`distros/fedora-installer/migrate/`, 1604 lines, stdlib, unit-tested in `test_import_units.py`) already translates `.service`/`.socket`/`.timer` (OnCalendar), specifiers, conditions, hardening, `Type=dbus`→`ready_bus_name`, and D-Bus activation overrides. It is system-oriented (system unit dirs, root specifiers). **It is not packaged** in any RPM (gap found 2026-09-22).
- Session D-Bus activation today: schema-dbus forks the activation file's `Exec=` itself (`sdbus_activate.h`), passing the broker's environment.
- `scripts/schema-systemd1-session.py` owns `org.freedesktop.systemd1` on the session bus: blind-forwards to the system-bus bridge, except `StartTransientUnit` which it forks itself.

## Design — decided

### 1. Two programs, one engine (Phase 1, PR A — PID 1 refactor, no behaviour change)

Move the supervisor engine out of `init.c` into shared compilation unit(s) linked by both `schema-init` and the new `schema-user`. `schema-init` stays PID 1 only (guard kept). The engine takes its paths and its PID-1-only hooks from a small context struct (or equivalent) rather than `#define`s and `getpid()==1` checks scattered through it: ctl socket path, notify socket path, shm name, timer stamp dir, cgroup root, services dir(s), and callbacks for PID-1-only behaviour (watchdog pet, PSI posture, shutdown/reboot). PID 1 behaviour must be byte-for-byte equivalent in effect: same boot, same ctl protocol, same re-exec blob compatibility.

This PR ships alone: `make test`, schema-vmtest boot, REEXEC old→new and new→old in VM, then a blakbox reboot, before anything builds on it.

*Greg proposed "share the .o files"; that understates it — the engine is file-static inside `init.c`, so this is a real extraction.*

### 2. `schema-user` (Phase 1, PR B)

New binary, runs as the user, one per uid.

- `prctl(PR_SET_CHILD_SUBREAPER, 1)` at start so double-forking services stay supervised.
- Runtime state under `$XDG_RUNTIME_DIR/schema-user/`: control socket `ctl.sock`, notify socket `notify`, generated services dir `services/`. Shm name `/schema-user-<uid>`. Persistent timer stamps under `$XDG_STATE_HOME/schema-user/timers` (default `~/.local/state/...`).
- No PID-1 work ever: no mounts, no watchdog, no modprobe, no reboot, no console. On SIGTERM: stop its services in reverse dependency order (same ordered-stop logic as PID 1 shutdown, minus the system parts), then `_exit(0)`.
- Cgroup root = its delegated subtree (§3). Each service gets `<subtree>/<name>` exactly as PID 1 does under `/sys/fs/cgroup/schema-init/`.
- Environment given to services: `HOME`, `USER`, `LOGNAME`, `SHELL`, `PATH`, `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus`, plus `environment.d(5)` replay (same set `zzz-environment-d.sh` builds). No `WAYLAND_DISPLAY`/`DISPLAY` in Phase 1 (that is Phase 2's `SetEnvironment`).
- Units that need the session bus (`Type=dbus`, `BusName=`, or ordered after `dbus.socket`/`dbus.service`) are gated until `$XDG_RUNTIME_DIR/bus` exists and accepts a connection. For a lingering user with no session they wait (shown as waiting, not failed).
- **Bus location is a precondition, not something `schema-user` discovers.** Phase 1 supports a session bus at `$XDG_RUNTIME_DIR/bus`, from either the schema-dbus broker or stock `dbus-daemon` (`schema-dbus-session-run.sh` already listens there in both modes). A host whose session uses an ad-hoc `/tmp/dbus-*` address (Optiplex today: older launcher via plain `dbus-run-session`) must be moved to `schema-dbus-session-run.sh` first, as its own step with a reboot verify (see Rollout).

### 3. Per-user cgroup delegation (Phase 1, in PR B or its own PR C)

schema-logind (root) creates the user's subtree before starting `schema-user`, using systemd's path so tools that parse `/proc/<pid>/cgroup` (sd-login's `sd_pid_get_user_unit` etc.) keep working:

`/sys/fs/cgroup/user.slice/user-<uid>.slice/user@<uid>.service/`

Delegation = chown that directory and its `cgroup.procs`, `cgroup.subtree_control`, `cgroup.threads` to the uid, enable the controllers PID 1 already enables at the root. `schema-user` is moved into it (write its pid to `cgroup.procs`) before it execs, then creates `init.scope`-style leaf for itself and per-service leaves (no-internal-process rule). Teardown: when `schema-user` exits and the subtree is empty, logind removes it.

PID 1 must not treat `user.slice` as foreign: check that the orphan/stray sweeps and shutdown (`container_cgroups_*`, `svc_signal_all`, shutdown ordering) handle it — at shutdown, `schema-user` instances get SIGTERM and their ordered-stop window before the final kill. If PID 1 needs a change for this, it rides with PR A's test discipline.

### 4. Lifecycle — owned by schema-logind (Phase 1)

- **Start:** when a uid gains its first session, and at logind startup for every user with `/var/lib/systemd/linger/<user>`. Not started if the kill switch is set. "Gains a session" means **either** path logind already knows: `CreateSession` **or** a new entry in `SESSIONS_DIR` (`/run/systemd/sessions/`) picked up by the existing poll (`schema-logind.py:2877`). The fleet's autologin desktops use the second path (`schema-plasma-autologin.sh:85` → `schema-session-register`), so it is the one Phase 1's done criteria depend on.
- **Never blocks:** spawned in the background; `CreateSession` returns without waiting.
- **Supervision:** logind restarts a crashed `schema-user` with backoff (e.g. 1s, 2s, 4s… capped), giving up after N crashes in a window and logging it. A crash never touches the session.
- **Stop:** when the user's last session closes and no linger file exists → after a **grace period (30 s)**, SIGTERM, wait for exit (bounded), clean the cgroup subtree. If a session for that uid appears within the grace period (the autologin `.svc` respawning the desktop with a new session id), nothing stops. Lingering users keep running.
- **logind restart / re-exec:** on startup, logind **adopts** any running `schema-user` it finds in `user.slice/user-<uid>.slice/user@<uid>.service/` (read `cgroup.procs`, match `/proc/<pid>/exe`) instead of starting a second one; adopted instances are supervised by polling their pid (they are not logind's children after a crash-restart). Never kill-and-replace on a logind restart — users' services must not bounce because logind re-exec'd (`kill -HUP` handoff, per the never-restart-session-daemons rule).
- **Linger:** implement `SetUserLinger` for real (create/remove `/var/lib/systemd/linger/<user>`, start/stop accordingly) and make `loginctl enable-linger` work through it. For a lingering user, logind also creates `/run/user/<uid>` at boot.

### 5. Unit ingestion — `schema-import --user` (Phase 1)

Extend `schema-import.py` with a user mode; **package it** (fix the 2026-09-22 gap) so `schema-user` can call it from `/usr/libexec/schema-init/`.

- Search path, highest first: `~/.config/systemd/user` → `/etc/systemd/user` → `/usr/lib/systemd/user` (plus `/usr/local/lib/systemd/user` if present). Masks (`→ /dev/null`) honoured. Drop-ins honoured as for system units.
- **Enabled set** = closure of `default.target`, `sockets.target`, `timers.target`, `basic.target` via their `.wants/`/`.requires/` in all three dirs, plus `WantedBy`/`RequiredBy` resolution as for system units. Units reachable only through session targets (see Non-goals) are skipped with reason "phase 2 session target".
- User specifiers: `%h`→`$HOME`, `%t`→`$XDG_RUNTIME_DIR`, `%S`→`$XDG_STATE_HOME`, `%C`→`$XDG_CACHE_HOME`, `%E`→`$XDG_CONFIG_HOME`, `%u`/`%U`, `%T`/`%V` as systemd.
- Output: `.svc` files into `$XDG_RUNTIME_DIR/schema-user/services/`, regenerated at every `schema-user` start and on `schema-ctl --user daemon-reload`. Generated files are plain text, `cat`-able, with the existing `# schema-import WARN:` lines for anything dropped.
- `.socket` → existing `listen=` / `service_listen_parse` path (PipeWire's `pipewire.socket`/`pipewire-pulse.socket` are the proving case). `.timer` → existing timer keys. `.path` → skipped with warning.

### 6. Skip list — "nothing starts twice" (Phase 1)

`/etc/schema-init/user-skip` (system-wide) and `~/.config/schema-user/skip` (per user): one unit name per line. Listed units are never generated; `schema-ctl --user status` shows them as `skipped (handled elsewhere)`. The installer/migration seeds the system file with what `plasma-session-start.sh` and system `.svc`s already provide (`pipewire.service`, `pipewire.socket`, `pipewire-pulse.service`, `pipewire-pulse.socket`, `wireplumber.service`) and the per-host extras found by survey (blakbox: `syncthing.service`). Retiring a workaround = remove the workaround **and** its skip line, reboot, verify (see Rollout).

### 7. `schema-ctl --user` (Phase 1)

`schema-ctl --user <cmd>` talks to `$XDG_RUNTIME_DIR/schema-user/ctl.sock` with the same protocol and output as system mode (`status`, `start`, `stop`, `restart`, `daemon-reload`, `status --json`). The `systemctl` shim maps `systemctl --user …` to it (today it falls through/fails), including `enable`/`disable` (writes the `.wants` symlink in `~/.config/systemd/user`, then daemon-reload) and `--user enable --now`.

### 8. Session D-Bus activation (Phase 1)

Applies only when the session broker is schema-dbus (gate file `/etc/schema-init/dbus-broker`; blakbox and Eli today). Under stock `dbus-daemon` the session daemon runs `Exec=` itself (no `--systemd-activation`), which is today's behaviour and stays unchanged in Phase 1.

For session activation files whose `SystemdService=` resolves to a unit `schema-user` has generated: activation goes through a helper (`schema-bus-activate --user <svc> <BusName> <Exec…>`, mirroring the system helper) that asks `schema-user` to start the service and waits for the bus name. **If `schema-user` is not reachable, or does not know the unit, the helper execs the original `Exec=` directly** — i.e. today's behaviour. Activation files whose `SystemdService=` does not map are untouched. Login-critical activations (portals, `kactivitymanagerd`) are already started directly by `plasma-session-start.sh` and are unaffected either way.

### 9. Python session shim (Phase 1)

`schema-systemd1-session.py`: `StartUnit`/`StopUnit`/`RestartUnit` for a unit `schema-user` knows → forward to `schema-user` (via its ctl socket), so there is one supervisor, not two. Everything else unchanged, including `StartTransientUnit`. Fully replaced in Phase 2.

### 10. Rollout and retiring workarounds (Phase 1)

Order per PR: `make test` + unit tests → schema-vmtest (boot, autologin login to desktop, kill-switch boot, `schema-user` crash-loop boot showing desktop unaffected) → blakbox reboot → Optiplex and Eli, one reboot each, desktop/Kodi up, 0 failed.

**Optiplex precondition:** before `schema-user` is enabled there, move Optiplex's session launch onto `schema-dbus-session-run.sh` (bus at `$XDG_RUNTIME_DIR/bus`) with the **schema-dbus broker** (Jonathan 2026-10-20 — same as blakbox and Eli; gate file `/etc/schema-init/dbus-broker`) — one reboot, desktop + Kodi + Frigate cams + EasyEffects audio verified.

Then, per host, one workaround at a time, each verified through a reboot:
1. Optiplex `obsidian-tunnel` (nothing to remove — just verify it runs).
2. blakbox Syncthing: remove `~/.config/autostart/syncthing.desktop` + skip line.
3. PipeWire stack: remove the `plasma-session-start.sh` block and the system `pipewire*.svc`/`wireplumber.svc` + skip lines. Real-time scheduling comes from **rtkit** (running on blakbox, Optiplex, Eli), which is how stock Fedora's user-unit PipeWire gets it; the system `.svc`'s `limit_rtprio=70`/`limit_memlock=4G` are not reproduced. Verify per host: PipeWire data-loop threads show `FF` in `ps -L -o cls`, no xruns in a 10-minute play, Kodi + EasyEffects audio on Optiplex. If rtkit doesn't grant RT on a host, that host keeps its system `.svc` and skip line.
4. blakbox `blakbox-backup.timer`: move from system `.svc` back to the user timer (it runs restic as the user — confirm before moving).
5. Report the remaining dead blakbox units (`openclaw-gateway`, `wifi-densepose`, `hermes-gateway`, `claude-brain`, `claude-sync`) to Jonathan **before** they start running — they've been dead for a while and may be obsolete; he decides enable/disable for each.

## Phase 2 — outline (own spec later)

Finish line: GNOME and stock `startplasma-wayland` log in unmodified on a test VM; most of `plasma-session-start.sh` deleted.

- **Session targets** as real sync points: `graphical-session-pre.target` → `graphical-session.target`, `plasma-core.target`/`plasma-workspace.target`, `gnome-session*.target`, with ordering and "reached" state.
- **Native session-bus `org.freedesktop.systemd1`** served by `schema-user` (or schema-dbus on its behalf): `StartUnit`, `StopUnit`, `ListUnits`, `GetUnit`, unit properties, `JobNew`/`JobRemoved` signals that gnome-session/startplasma wait on, `SetEnvironment`/`UnsetEnvironment`/`import-environment` so `WAYLAND_DISPLAY`/`DISPLAY` reach later units, `StartTransientUnit` for `app-*.scope`/`app-*.service` with real scopes. Retires `schema-systemd1-session.py`.
- Session bus itself as a user unit (`dbus.socket`) under `schema-user`, so lingering users get a bus.
- Then the login moves onto the supervisor — VM only until GNOME and stock Plasma are clean there; hardware after.

## Left to the builder

- Exact shape of the engine context struct / callback seam in PR A, and which file(s) the engine moves to — constrained by "no PID 1 behaviour change, re-exec blob compatible".
- Whether cgroup delegation lands in PR B or a separate PR C.
- logind restart backoff numbers and crash-window limits.
- How `schema-user` waits on the bus (inotify on `$XDG_RUNTIME_DIR` vs. reuse of the `ready_*` path watchers).
- Test layout for `schema-import --user` (extend `test_import_units.py` with a fake user tree via `MIGRATE_ROOT`).

## Claire's calls (Jonathan can override)

- Binary name `schema-user`; runtime dir `$XDG_RUNTIME_DIR/schema-user/`.
- systemd-compatible cgroup path `user.slice/user-<uid>.slice/user@<uid>.service/`.
- Logout without linger stops the user's services (matches systemd).
- logind owns `schema-user`'s lifecycle (not a PID 1 template service — schema-init has no templates).
- Kill switch names: `/etc/schema-init/user-supervisor.disabled` and `schema.user=0`.
- Units reachable only via session targets are deferred to Phase 2 (so `gammastep` stays off for now).
- The long-dead blakbox units are not auto-revived; Jonathan decides each.
- Session start/end detection includes the `SESSIONS_DIR` poll path; 30 s grace on last-session-close.
- logind adopts running `schema-user` instances on restart/re-exec; never kill-and-replace.
- PipeWire real-time via rtkit (stock Fedora), verified per host; fall back to keeping the system `.svc` on any host where rtkit doesn't deliver.
- Session D-Bus activation routing only under schema-dbus; stock `dbus-daemon` hosts keep today's activation.

## Carried into PR B (from the PR A final review, 2026-10-20)

PID-1-only behaviour still inside `engine.c` that `schema-user` must parameterise or bypass:
- `service.h` `cgroup_path[128]`: `user.slice/user-<uid>.slice/user@<uid>.service/<root>/<name>` can exceed 128 → widen `cgroup_path` or bound root length when set.
- `ctl_poll` refuses mutating verbs when `peer_uid != 0` → must accept the supervisor's own uid.
- `./run/schema-init.sock` and `./run/schema-init/env` cwd fallbacks; `engine_load`'s `/proc/cmdline` + `/etc/schema-init/hardening-default` reads and `./services` fallback.
- `timer_stamp_write` creates only the last component of `timer_stamp_dir` (and hard-codes `mkdir /var/lib/schema-init`) → `mkdir -p` semantics for `$XDG_STATE_HOME/schema-user/timers`.
- `ctl_init` chowns the socket to `0:schema`; log prefixes are hard-coded `[schema-init]`.
- `get_poll_timeout`: `psi_fd < 0` forces the 250 ms tick — needs a flag so `schema-user` sleeps to real deadlines.
- `engine.h` exports generic-named globals (`psi_fd`, `watchdog_fd`, `reexec_check_*`) — watch for clashes in the new binary.
- `service_notify_sock` must be set alongside `engine_paths.notify_sock`.
