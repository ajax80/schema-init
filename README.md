# schema-init

<p align="center">
  <a href="assets/schema-init-trailer.mp4"><img src="assets/schema-init-trailer.gif" alt="schema-init — 30-second trailer" width="640"></a>
</p>
<p align="center"><sub>▶ 30-second trailer — <a href="assets/schema-init-trailer.mp4">watch in full resolution, with sound</a></sub></p>

A minimal PID 1 init system for Linux that supervises services through a weight-state machine instead of unit files and dependency graphs.

No systemd. No OpenRC. No journal daemon. No socket activation engine. Just a statically linked binary that mounts your filesystems, spawns your services in dependency order, and watches them — then gets out of the way.

**PID 1 footprint: 1.2 MB RSS on a minimal boot, 3.3–4.0 MB running a 47-service KDE desktop — one thread, in every case.** Every footprint figure in this README names the machine, the build and the service count it was measured on: see [PID 1 RSS — every measurement](#pid-1-rss--every-measurement).

---

## What it gives back

systemd isn't just PID 1 — it's a constellation of always-on daemons: `journald`, `systemd-logind`, `dbus-broker`, `systemd-resolved`, resident `udevd` workers, timers firing on their own schedule. Each one holds RAM and wakes the CPU whether or not you're using it. schema-init replaces PID 1 with a single static binary and **does none of that** — no journal database, no socket-activation engine, no background event loops. What that machinery was holding comes back to you.

**Your RAM comes back.** On identical hardware running the identical desktop, schema-init frees roughly **half a gigabyte of RAM** that systemd's daemon stack was sitting on (~1.1 GB used at desktop vs ~1.6–2.0 GB — see [Real numbers](#real-numbers)), and idle swap drops from hundreds of MB to **zero**. In lived terms that is the difference between *a few browser tabs plus one other program before the machine starts thrashing* and **two or three browsers with ~20 tabs each and a game running at the same time** — same RAM, no upgrade. The computer you already own effectively gets bigger.

**Your power comes back.** With no ambient timer wakeups holding the cores awake, the CPU actually reaches its deepest hardware sleep state: measured **92–99% C10 residency** and **~1.25 W full-SoC package draw** at a working desktop, idle load average **0.03** (vs 0.10–0.20 under systemd). Those figures are read from Intel RAPL hardware energy counters, not estimated. Per machine it is a small, honest number — but it is *structural*, paid back every second of every idle hour. schema-init's part is simply removing the constant wakeups that keep silicon out of deep sleep in the first place.

> **On extrapolating this:** don't. These are single-node measurements on one i3 laptop. An init system's own power draw is a tiny slice of a server's total, so multiplying a per-node idle delta by a fleet size produces a number that will not survive contact with anyone who runs real hardware. If schema-init saves money at scale, the levers are **density, footprint, boot time, attack surface and determinism** — not init power draw.

**The machine goes quiet, not just lean.** One PID-1 thread instead of 20–30. A tick loop that sleeps *indefinitely* once services are stable — nothing wakes it on a schedule. No journal flush, no D-Bus polling, no watchdog chatter. The hardware is allowed to actually rest.

This isn't theory or a benchmark rig — it's a salvaged Dell Inspiron (Intel i3, 4 GB) that swapped constantly under systemd and now runs a full desktop with room to spare under schema-init. Older and low-RAM machines benefit the most: the daemons you delete are the exact ones a small machine can least afford.

**PID 1 footprint: 1.2 MB RSS on a minimal boot, 3.3–4.0 MB running a 47-service KDE desktop — one thread, in every case.** Every footprint figure in this README names the machine, the build and the service count it was measured on: see [PID 1 RSS — every measurement](#pid-1-rss--every-measurement).

---

## How it works

Every service moves through a state machine driven by probes. Before a service is spawned, `schema-init` probes the system — is the binary present? Are dependencies stable? Is there enough memory? The probe returns a flag word. The state machine decides what comes next.

```
                  ┌─────────────────────────────────────┐
                  │                                     │
            NEW_PROCESS                                 │
                  │                                     │
           F8 probe passes                              │
                  │                                     │
            FULL_TRUST ──── stable 10s ──── FUNDAMENTAL │
                  │                         SETTLED     │
                  │                                     │
             (oneshot exit 0)                           │
                  │                                     │
              PERFECT                                   │
                                                        │
            ── on death ──                              │
                  │                                     │
             RECOVERY ◄──────────────────────────────── ┘
                  │
           F9 probe fails
                  │
             FRICTION
                  │
           F6 probe fails
                  │
             DORMANT  (75 — backoff anteroom: 5m → 10m → 20m → 40m → 60m)
                  │
         (non-critical, 5 cycles exhausted)
                  │
             EXCISED  (76 — gate closes)
```

Three probe families:

| Probe | Asked when | Checks |
|-------|-----------|--------|
| **F8** | Before first spawn | Binary exists, deps stable, memory safe, permissions met |
| **F9** | After death | Retry budget, cooldown window, memory, escalation path |
| **F6** | After recovery fails | Last-chance: can we even attempt a restart? |

Services marked `critical=1` never reach EXCISED — they enter DORMANT and retry at 1-hour intervals indefinitely. Non-critical services excise after 5 dormant cycles (~75 minutes). A dep marked `critical=1` that is EXCISED still blocks its dependents. A non-critical EXCISED dep is skipped — dependents proceed without it.

---

## Repository layout

If you're reading the source to evaluate it, start here. The whole init is ~2,500 lines of C with no external dependencies.

**Read these first, in this order:**

| File | Lines | What it is |
|------|-------|------------|
| `init.c` | ~1,370 | PID 1 itself. Mounts pseudo-filesystems, reaps children, runs the supervise loop, handles signals and shutdown. The spine — everything below is called from here. |
| `schema.c` / `schema.h` | ~70 | The weight-state machine. Pure state transitions; a service's "weight" is the popcount of its probe flag word. This is *the schema* — the single source of truth for what every state means. |
| `service.c` / `service.h` | ~680 | Parses `.svc` files, spawns services, runs the F8/F9/F6 probes, and drives the recovery → backoff → excision arc. |
| `group.c` / `group.h` | ~150 | Aggregates a `.grp` of services into one worst-case state, so a stack (network, display) promotes and fails as a unit. |

**Supporting binaries:**

| File | What it is |
|------|------------|
| `schema-ctl.c` | The CLI client. Talks to PID 1 over the `/run/schema-init.sock` UNIX socket — `schema-ctl status`, `restart`, etc. |
| `schema-subreaper.c` | ~50-line helper that sets `PR_SET_CHILD_SUBREAPER` so a service can adopt its own orphaned grandchildren instead of dumping them on PID 1. |
| `schema-journal-sink.c` | Opt-in Track B compatibility shim. Provides journald's three ingestion sockets (`/dev/log`, `/run/systemd/journal/{socket,stdout}`) and drains them to a plain logfile so foreign libsystemd/syslog software finds a journald-shaped endpoint. No journal DB, no `journalctl`. schema-init never needs it to boot. See `docs/journal-sink-design.md`. |
| `schema_shm.h` | The shared-memory interface — PID 1 publishes live service state here so external tools can read it without polling the socket. |
| `schema-board.c` | Read-only board that renders every service's weight-state in its LED colour, reading the shm export above rather than the control socket — so it keeps working when the socket or the desktop is wedged. `--once` prints one frame and exits. Reads a world-readable `0644` shm segment, so unlike `schema-ctl` it **needs no root**. `--tty /dev/tty8` paints a dedicated console; note that VT switching does not currently repaint on a graphical system — see [Recovery console](#recovery-console). Increments 1–2 of the limp-mode recovery surface (`docs/superpowers/specs/2026-06-14-limp-mode-design.md`). |

**Directories:**

| Dir | What's inside |
|-----|---------------|
| `services/` | The reference service set — real `.svc` and `.grp` files for `sshd`, `dbus`, `udev`, `network-manager`, `display-manager`, and the `network-stack` / `display-stack` groups. Copy these as your starting templates. |
| `desktop/` | `schema-desktop.c` — an SDL2 live visualizer that maps `schema_shm.h` into an 8-node grid and shows every service's weight-state in real time. This is how you *watch* the state machine run. |
| `scripts/` | Build and integration tooling: `make-iso*.sh` / `make-usb.sh` / `fix-usb.sh` (bootable media), `schema-logind.py` (a logind compatibility shim), and `verify_traceability.py` (IEC 62304 requirement traceability). |
| `distros/` | Per-distribution profiles — `fedora-kde/` and `raspberry-pi-zero-w/`. Each carries the service files and boot glue that distro needs. |
| `docs/`, `assets/` | Documentation and images. |

Top-level: `setup.sh` (newcomer bootstrap — dep check, desktop-environment detection, GRUB entry generation) and `Makefile` (static build; see [Building](#building)).

---

## Service files

Drop a `.svc` file in `/etc/schema-init/services/`. One key=value per line:

```ini
name=sshd
exec=/usr/sbin/sshd
args=-D
needs_root=1
```

```ini
name=display-manager
exec=/usr/sbin/lightdm
dep=dbus
dep=udev
needs_root=1
critical=1
```

```ini
name=network
exec=/usr/local/bin/net-setup
oneshot=1
```

**Keys:**

| Key | Default | Description |
|-----|---------|-------------|
| `name` | *(required)* | Service name — used in logs, dep resolution, and schema-ctl commands |
| `exec` | *(required)* | Absolute path to the binary to execute |
| `args` | — | Argument string (repeat the key for multiple args) |
| `dep` | — | Dependency by name (repeat for multiple deps; can name a service or a group) |
| `oneshot` | `0` | Exit 0 → PERFECT and don't restart; exit non-zero → RECOVERY arc |
| `needs_root` | `0` | Abort spawn if uid ≠ 0 |
| `critical` | `0` | If `1`: service never reaches EXCISED — stays DORMANT at 1h retry indefinitely. Also: if this service is EXCISED, its dependents are hard-blocked. |
| `no_restart` | `0` | Any death → EXCISED immediately; no recovery arc |
| `max_restarts` | `5` | Maximum number of times to attempt restarting a service before entering EXCISED or backoff |
| `stable_secs` | `10` | Seconds process must stay alive before FULL_TRUST promotes to FUNDAMENTAL. Set lower for fast services; use `ready_path` instead when possible |
| `ready_path` | — | Filesystem path that, when it exists, triggers immediate FULL_TRUST→FUNDAMENTAL promotion. Falls back to `stable_secs` if the path never appears. In FUNDAMENTAL it also acts as a liveness probe: if the path *disappears*, the service is killed and backed off. The disappearance check only arms once the path has been seen at least once — a service promoted by `stable_secs` before its path exists won't be falsely killed. For services slower than `stable_secs` to come up (e.g. NetworkManager writing `resolv.conf`), set `stable_secs` generously so promotion doesn't outrun the path. |
| `watchdog_timeout_ms` | `0` | Dead Man Token window in milliseconds. Service must call `schema-ctl pet <name>` within this window or PID 1 stops kicking `/dev/watchdog` and the hardware resets. Use for `critical=1` real-time processes. `0` = disabled. |
| `cpu_limit` | `0` | Percent of one CPU core (1–100) enforced via cgroupv2 `cpu.max`. Written before child exec. `0` = unlimited. |
| `mem_limit` | `0` | Memory hard cap in MB via cgroupv2 `memory.max`. OOM inside the cgroup kills the service, not the system. Written before child exec. `0` = unlimited. |
| `priority` | `standard` | CPU contention class via cgroupv2 `cpu.weight`: `critical` (weight 1000), `standard` (100), `peripheral` (10). Proportional share — only takes effect when cores are saturated; idle services are never penalized. The analog of systemd's `CPUWeight=`. Children inherit the service's cgroup, so tagging a session leader (e.g. `display-manager`) elevates its whole subtree, compositor included. |
| `cpuset` | — | CPU affinity list pinning the service to specific cores via cgroupv2 `cpuset.cpus` (e.g. `2,3` or `4-7`). Constrains *where* the service may run, complementing `priority`/`cpu_limit` which govern *how much*. The analog of systemd's `AllowedCPUs=`. Requires the kernel's cgroupv2 `cpuset` controller (delegated automatically); if absent the setting no-ops. Empty = unconstrained (inherits the parent's CPUs). Useful for isolating a latency-sensitive control loop from cores that bursty background work hammers. |
| `cpuset_partition` | `member` | Exclusivity tier for the `cpuset=` cores via cgroupv2 `cpuset.cpus.partition`. `member` (default) = plain pinning, cores stay shared (no-op). `root` = the cores become an *exclusive* partition (no other service may run on them) while still being scheduler-load-balanced. `isolated` = exclusive **and** removed from the scheduler's load balancer — dynamic `isolcpus=`, no kernel cmdline needed; the target for a latency-critical control loop (the Ungulate Leg) or the audio path. Implemented as a cgroup v2 *remote partition*: schema-init reserves the cores in its own `cpuset.cpus.exclusive` and the service forms the partition, so the other services are unaffected. If the kernel rejects the partition (overlapping cores between two isolated services, or no cpuset controller) the service silently degrades to plain `cpuset=` pinning and a `HAZARD` line is logged — boot is never blocked. Requires a non-empty `cpuset=`; setting it alone is ignored with a warning. |
| `allowed_slot_min` | `-1` | Minimum hardware slot ID (inclusive) this service is permitted to run on. Checked against `SLOT_ID` env at spawn time. `-1` = unconstrained. |
| `allowed_slot_max` | `-1` | Maximum hardware slot ID (inclusive). If `SLOT_ID` falls outside `[allowed_slot_min, allowed_slot_max]`, spawn is refused with a `HAZARD` log and `SVC_NO_RESTART` is set — the service will not retry. Both min and max must be ≥ 0 to activate the gate. |
| `on_boot_sec` | `0` | Makes the service a **timer**: seconds after boot before the first fire (`0` = at boot). Implies `oneshot=1` — the service runs, exits, and re-arms. The analog of systemd's `OnBootSec=`. See [Timers](#timers) below. |
| `on_active_sec` | `0` | Timer period: seconds after each completion before the next fire. Measured from completion (like systemd's `OnUnitInactiveSec=`), so a slow run never overlaps itself. Implies `oneshot=1`. |
| `start_timeout_sec` | `90` for oneshots, `0` otherwise | Max seconds a service may sit in `FULL_TRUST` without promoting before it is killed and routed into the recovery arc — so a hung boot service can't stall its dependents. **Defaults on for oneshots** (the only services that can hang the chain; daemons promote via `stable_secs`). **Timers are exempt** (may run long). `0` disables. The analog of systemd's `TimeoutStartSec=`. |
| *(default)* | | Services restart automatically through the F9/F6 recovery arc unless `no_restart` or `oneshot` is set |

A full example using readiness probes:

```ini
name=dbus
exec=/usr/bin/dbus-daemon
args=--system
args=--nofork
needs_root=1
stable_secs=2
ready_path=/run/dbus/system_bus_socket
```

### Service templates

For fleets of identical services — e.g. 49 joint controllers on an exoskeleton — define config once and symlink instances:

```sh
# template — write once
/etc/schema-init/services/motor@.svc

# instances — zero-byte symlinks; suffix becomes $INSTANCE in the child
ln -s motor@.svc /etc/schema-init/services/motor@0.svc
ln -s motor@.svc /etc/schema-init/services/motor@12.svc
ln -s motor@.svc /etc/schema-init/services/motor@48.svc
```

At boot, `motor@.svc` is skipped as a non-spawnable template. Each `motor@N.svc` symlink loads config from the template and spawns the binary with `INSTANCE=N` in the child environment. The motor controller reads `$INSTANCE` to determine its joint index, SPI bus address, or any other per-instance identity — no per-node config files required.

If a node runs the bare template directly (e.g. on a slot-detected Pi Zero W 2 where the node's identity comes from GPIO strapping), `INSTANCE` falls back to `SLOT_ID` from `/run/schema-init/env`. One SD card image serves the entire fleet.

**AllowedSlot gate** — for hardware deployments where running the wrong firmware on the wrong node is a physical hazard, add slot constraints to the template:

```ini
name=motor
exec=/usr/local/bin/motor-ctrl
allowed_slot_min=16
allowed_slot_max=27
```

If `SLOT_ID` is outside the declared range at spawn time, `schema-init` logs a `HAZARD` line, refuses the spawn, and sets `SVC_NO_RESTART`. The process never runs. Project Daedalus slot map:

| Slot range | Joint |
|------------|-------|
| 0–7 | Hip Left |
| 8–15 | Hip Right |
| 16–21 | Knee Left |
| 22–27 | Knee Right |
| 28–33 | Ankle Left |
| 34–39 | Ankle Right |
| 40–43 | Toe Left |
| 44–47 | Toe Right |
| 48 | Supervisor |

Dependencies are resolved by name at load time. A service stays in `NEW_PROCESS` until all its deps reach `FUNDAMENTAL`, `SETTLED`, or `PERFECT`. A dep name can refer to either a service or a group (see below).

### Group files

Drop a `.grp` file in the same services directory to create a named group. Services can depend on a group name just like a service name.

```ini
name=storage
member=lvm
member=cryptsetup
member=mount-data
```

A group's state is the worst-case view of its members:
- Any member EXCISED → group is EXCISED
- Any member in FRICTION/RECOVERY → group reflects that
- All members FUNDAMENTAL or better → group is FUNDAMENTAL
- All members PERFECT → group is PERFECT

Maximum 16 groups, 8 members per group. Names and members are matched at load time.

### Timers

Add `on_boot_sec` and/or `on_active_sec` to any `.svc` to make it **periodic** — no separate `.timer` file, no second unit to link. The service *is* the timer. This replaces `cron` and systemd `.timer` units with the same `.svc` you already wrote.

```ini
name=fstrim
exec=/usr/sbin/fstrim
args=-a
needs_root=1
on_boot_sec=600        # first fire 10 min after boot
on_active_sec=86400    # then every 24 h after each completion
```

A timer is a oneshot that re-arms on a `CLOCK_MONOTONIC` deadline instead of staying terminal at PERFECT:

- It boots into PERFECT (as if it already ran), first fire at `boot + on_boot_sec`.
- On fire it re-enters NEW_PROCESS — so **dependencies are still honored** and it waits for its deps exactly like any service.
- When it exits, it re-arms for `now + on_active_sec` **regardless of exit code** (cron semantics — a failed run is not retried in a loop; it runs again next window). The exit is logged `timer-done` or `timer-failed`.

**Run-once:** set only `on_boot_sec` (leave `on_active_sec` unset) and the service fires exactly once, `on_boot_sec` seconds after boot, then stays terminal — a deferred startup job rather than a repeating one.

The period is measured from completion, so a slow job never overlaps itself. Fires on the 250 ms tick (±1 tick) — cron-class precision, not sub-second. For real-time work use `watchdog_timeout_ms` and the control loop instead.

**Wall-clock timers** — set `on_calendar=HH:MM` to fire at a fixed local time every day, the way you'd write a cron line. This is the form you want for "3am backup", "midnight log rotation", "nightly cert renewal":

```ini
name=nightly-backup
exec=/usr/local/bin/backup.sh
needs_root=1
on_calendar=03:00     # fire at 03:00 local time, every day
```

`on_calendar` re-evaluates the wall clock on every fire, so it tracks `CLOCK_REALTIME` (not monotonic) — DST shifts and NTP clock steps self-correct each cycle rather than drifting. Time is local (`/etc/localtime`). Malformed values (`want HH:MM`, `00:00`–`23:59`) are logged and ignored, never scheduled.

**Catch-up after downtime** — by default a job missed while the machine was off simply runs at its next occurrence. Add `persistent=1` to run it **once at boot** instead, if its scheduled time passed while the system was down (systemd `Persistent=true`):

```ini
name=nightly-backup
exec=/usr/local/bin/backup.sh
needs_root=1
on_calendar=03:00
persistent=1          # if 03:00 was missed while off, run at next boot
```

Last-run is stamped to `/var/lib/schema-init/timers/<name>.stamp`; at boot, if the most recent `HH:MM` occurrence is newer than that stamp, the timer fires immediately (logged `timer-catchup`) instead of waiting. A never-run timer is seeded rather than replayed, so enabling one doesn't trigger a surprise fire on first boot. `persistent=1` only applies to `on_calendar` timers; on an interval timer it's logged and ignored.

**Not yet implemented:** richer calendar forms (day-of-week, multiple times per day). See `docs/timers-design.md`.

---

## schema-udev

`schema-udev` is a native uevent→schema→action daemon — a small, purpose-built alternative to udev rules for the specific devices you care about (an ESP32 over USB-serial, an RFID reader, a sensor board), running **alongside** real `systemd-udevd`, not replacing it. udevd still owns `/dev` population, symlinks, and driver binding; `schema-udev` just watches the same kernel uevent stream and runs your hook scripts when a match fires.

It binds **kernel netlink group 1** (`UDEV_MONITOR_KERNEL`), never group 2 (`UDEV_MONITOR_UDEV`) — group 2 is udevd's own processed-event multicast, consumed by libudev/PipeWire for device enumeration. `schema-udev` only listens to the raw kernel group, so it has zero observed impact on PipeWire or desktop hotplug. Datagrams are checked against the kernel's `SCM_CREDENTIALS` (uid 0, pid 0) before being parsed, so a spoofed unprivileged uevent is dropped.

**Rule files** live in `/etc/schema-init/dev/*.dev`, one `key=value` per line:

```ini
name=esp32-serial
match_subsystem=tty
match_product=10c4/*
symlink=esp32
on_add=/usr/local/bin/esp32-up.sh
on_remove=/usr/local/bin/esp32-down.sh
```

- `match_*` keys map to raw kernel uevent keys (`match_subsystem` → `SUBSYSTEM`, `match_product` → `PRODUCT`, etc.), **ANDed together** — a rule only fires when every `match_*` key it declares matches. Values support `fnmatch(3)` globs (`10c4/*`).
- `symlink=<name>` creates a stable symlink `/dev/schema/<name>` → `/dev/<DEVNAME>` on `add`, unlinking it on `remove`. Created atomically prior to `on_add` hook execution. Name must be single-level (no `/` or `..`, max 63 chars) under the parallel `/dev/schema/` namespace to avoid writer contention with systemd-udevd.
- `on_add` / `on_remove` are hook commands run via `/bin/sh -c` with the full uevent exported as environment variables — `ACTION`, `DEVNAME`, `DEVPATH`, `PRODUCT`, `MODALIAS`, and whatever else the kernel sent.
- **Coldplug at startup**: On launch, `schema-udev` performs an in-process physical sysfs walk (`/sys/devices`) to synthesize events for devices already present at boot and fire `on_add` rules and symlinks without touching netlink or `/sys/*/uevent` files (ensuring zero `systemd-udevd` or desktop disturbance).
- Comments must be on their own line (`#` as the first non-blank character). There is no inline-comment stripping — a trailing `# note` after a value becomes part of the value. See `assets/example.dev` (fully inert — every line commented, safe to drop in as a template) and copy it to `/etc/schema-init/dev/<name>.dev` to activate.
- `SIGHUP` reloads all rule files from disk without restarting the daemon (`schema-ctl reload` or `kill -HUP` on its pid).
- Raw kernel (group 1) uevents deliver `DEVNAME` **without** the `/dev/` prefix (e.g. `DEVNAME=ttyUSB0`, not `/dev/ttyUSB0`) — don't anchor `match_devname` globs to `/dev/`, and hooks see `$DEVNAME` the same unprefixed way. Prefer keying on `match_subsystem` + `match_product` (vid/pid), as in the example above.

### Phase 3 (interop mechanism — built, not yet active)

schema-udev carries pure encoders for the two formats a future udevd
retirement needs: the **libudev monitor** netlink frame (group 2) and the
**`/run/udev/data`** device-database record. They are unit-tested against
real captured frames but are **not wired into the daemon** — schema-udev
neither broadcasts on group 2 nor writes `/run/udev` while systemd-udevd
runs (doing so would double libudev events / corrupt udev's database).
Activating them is a separate, deliberate cutover, not part of this build.

---

## State glossary

| State | Meaning |
|-------|---------|
| `NEW_PROCESS` | Queued. Waiting for all deps to reach FUNDAMENTAL. No spawn attempt yet. |
| `FULL_TRUST` | Spawned. Watching — promotes to FUNDAMENTAL when `ready_path` exists or `stable_secs` elapses, whichever comes first. |
| `FUNDAMENTAL` | Stable. Load-bearing. Other services can depend on it. |
| `SETTLED` | Stable, non-critical. Satisfies deps but generates no friction warnings if lost. |
| `RECOVERY` | Died unexpectedly. F9 probe running. May re-queue or escalate. |
| `FRICTION` | Recovery failed. F6 last-chance probe running. |
| `DORMANT` | F6 failed. Exponential backoff: 5m→10m→20m→40m→60m. Re-queues on wake. `critical=1` services never leave this toward EXCISED. |
| `EXCISED` | Permanently removed. Non-critical only, after 5 dormant cycles. Gate closes. |
| `PERFECT` | Oneshot service exited 0. Terminal success. |

---

## Shutdown

schema-init handles shutdown signals from userspace or the kernel:

```sh
sudo kill -TERM 1   # poweroff
sudo kill -INT 1    # reboot
```

On SIGTERM, schema-init sets system state to shutdown, sends SIGTERM to all child processes, waits 500ms for clean exit, then calls `reboot(RB_POWER_OFF)`.

On SIGINT, same sequence ends with `reboot(RB_AUTOBOOT)`.

The 500ms hold is intentional — it gives any running desktop or display manager time to render a shutdown state before the process tree is torn down.

---

## Known limitations

These are real gaps, not future features being teased:

- **No socket activation** — services must manage their own sockets. There is no systemd-style socket hand-off (`LISTEN_FDS`).
- **Log rotation needs a timer you schedule.** The `logrotate` config ships; nothing here fires it. See [Logs](#logs).
- **`schema-logind.py` is a stub with one session object, not one per session.** It implements enough of `org.freedesktop.login1` for a Wayland compositor to take KMS and hand it back on a VT switch (see [Recovery console](#recovery-console)), but it does not model multiple concurrent seats or sessions. It also does not set `KDSKBMODE = K_OFF`, deliberately — if the daemon died while `K_OFF` were set, the console keyboard would stay dead — so keystrokes can still leak to the tty underneath a compositor.

---

## Filesystem setup

schema-init does not parse `/etc/fstab`. On boot it mounts the pseudo-filesystems directly:

| Mount | Type | Notes |
|-------|------|-------|
| `/` | remount rw | Kernel mounts rootfs read-only for fsck; schema-init remounts it writable before anything else |
| `/proc` | proc | nosuid, nodev, noexec |
| `/sys` | sysfs | nosuid, nodev, noexec |
| `/dev` | devtmpfs | nosuid, strictatime |
| `/dev/pts` | devpts | nosuid, noexec, `gid=5,mode=620,ptmxmode=666` — without it there are no PTYs and every terminal emulator fails to start |
| `/dev/shm` | tmpfs | nosuid, nodev, mode=1777 — POSIX shared memory |
| `/run` | tmpfs | nosuid, nodev, mode=0755 |
| `/sys/fs/cgroup` | cgroup2 | nosuid, nodev, noexec, relatime |

After mounting `/dev`, schema-init creates four symlinks that devtmpfs does not provide and a userspace init is expected to make itself:

| Link | Target |
|------|--------|
| `/dev/fd` | `/proc/self/fd` |
| `/dev/stdin` | `/proc/self/fd/0` |
| `/dev/stdout` | `/proc/self/fd/1` |
| `/dev/stderr` | `/proc/self/fd/2` |

Without these, bash process substitution (`< <(...)`) and any `/dev/stdin`-style redirect fail — a gap that surfaces in ordinary shell scripts long before it surfaces anywhere in the init itself.

schema-init also creates `/run/log/schema-init/` at boot. Each service's stdout and stderr are redirected there automatically (see Logs).

If your system needs additional mounts (data partitions, network filesystems), run them as `oneshot` services before your other services depend on them.

> **Mount by `UUID=` or `LABEL=`, never `/dev/sdX`.** The kernel assigns `sda`/`sdb`/… in detection order, which can change between boots — so a oneshot that mounts `/dev/sdb1` may silently land on the wrong physical disk, swapping two data drives and pointing every absolute path at the wrong filesystem. `/etc/fstab` under systemd hid this by mounting by UUID for you; schema-init doesn't read fstab, so do it explicitly: `mount UUID=1b7d654f-… /mnt/data` (or `LABEL=`). The same applies to the root `LABEL=`/`PARTUUID=` on the kernel cmdline.

---

## Building

```sh
make
```

Produces a fully static binary — no glibc version dependency, runs on any Linux kernel. Tested on:

- Debian Bookworm, kernel 6.1, x86_64 — headless and Cinnamon desktop
- Fedora 44, kernel 7.0, x86_64 — full KDE Plasma desktop, btrfs subvolume boot

**Cross-compile for aarch64 (ARM — Ungulate Leg target):**

```sh
make aarch64
```

Requires `aarch64-linux-gnu-gcc`. On Fedora: `sudo dnf install gcc-aarch64-linux-gnu`. Produces static `schema-init-static`, `schema-ctl`, and `schema-subreaper` binaries. Override sysroot with `SYSROOT=/path/to/sysroot make aarch64`.

**ARM bare-metal (Pi Zero W, armv6l):**

Fedora's `arm-linux-gnu-gcc` cross-compiler does not ship an arm sysroot. Compile natively on the Pi:

```sh
sudo apt install git gcc make
git clone https://github.com/ajax80/schema-init
cd schema-init && make
```

The `armhf` Makefile target exists for environments that have a full arm sysroot available.

**schema-desktop (optional SDL2 monitor):**

```sh
make desktop
sudo cp desktop/schema-desktop /usr/local/bin/schema-desktop
```

Requires `SDL2` and `SDL2_ttf`. On Fedora: `sudo dnf install SDL2-devel SDL2_ttf-devel`. Reads live service state from PID 1's shared memory segment — run it from the desktop after login, or drop `distros/*/config/autostart/schema-desktop.desktop` into `~/.config/autostart/` to launch it automatically.

```sh
# install as PID 1 — symlink approach (distro-compatible)
cp schema-init /sbin/schema-init
ln -sf /sbin/schema-init /sbin/init

# or pass to kernel directly via GRUB
linux /boot/vmlinuz root=LABEL=my-root init=/sbin/schema-init
```

### GRUB setup

**Option A — symlink** (`/sbin/init` → `/sbin/schema-init`): works with any distro GRUB config, no kernel cmdline change needed. Replace your distro's init binary or point the symlink.

**Option B — explicit init= in GRUB**: add `init=/sbin/schema-init` to the kernel line in `/etc/default/grub`, then `grub-mkconfig -o /boot/grub/grub.cfg` (Debian/Ubuntu) or `grub2-mkconfig -o /boot/grub2/grub.cfg` (Fedora).

**Option C — custom GRUB menu entry**: create a separate entry that leaves the distro default untouched:

```
# /boot/grub/custom.cfg  (included automatically by grub.cfg)
menuentry 'schema-init' {
    search --no-floppy --label --set=root schema-root
    linux   /boot/vmlinuz-$(uname -r) root=LABEL=schema-root rw quiet init=/sbin/schema-init
    initrd  /boot/initramfs-$(uname -r).img
}
```

Option C is the safest for dual-boot or first-time installs — it leaves the existing systemd entry intact as a fallback.

**Kernel cmdline words are safe.** The kernel hands PID 1 every boot-cmdline token it didn't consume (`rhgb`, `quiet`, `splash`, `plymouth.debug`, …), so leave your usual options in the kernel line — schema-init ignores them when it runs as PID 1. A services directory other than the default `/etc/schema-init/services` can only be set by hand-running the binary (`schema-init /path/to/services`), never via the kernel cmdline.

### Replacing a running init (without reboot)

The init binary cannot be overwritten while running (`text file busy`). Use the copy-then-move trick:

```sh
cp schema-init /sbin/schema-init.new
mv /sbin/schema-init.new /sbin/schema-init
```

`mv` replaces the directory entry atomically without touching the inode that the kernel holds open. The new binary takes effect on next boot.

---

## Real numbers

Tested on Dell Inspiron 3542 (Intel Core i3, 4GB RAM) running full Cinnamon desktop:

| Metric | schema-init | systemd (same hardware, Fedora) |
|--------|-------------|----------------------------------|
| PID 1 RSS | see [PID 1 RSS — every measurement](#pid-1-rss--every-measurement) | *(not measured on this machine)* |
| PID 1 threads | **1** | 20–30+ |
| RAM used at desktop | **~1.1 GB** | ~1.6–2.0 GB |
| Swap used | **0 MB** | 200–500 MB |
| Time to desktop | **~20.7s** | slower |

The gap is structural. schema-init spawns your services and then sits in a 250ms tick loop. There is no journal daemon, no dbus-broker, no socket activation layer, no unit file parser running in the background.

Boot timing breakdown (Dell Inspiron 3542, Debian Bookworm, kernel 6.1.0-49, times relative to PID1 start):

```
kernel → PID 1:    6.968s
dbus               1.761s   (ready: /run/dbus/system_bus_socket)
elogind            2.739s   (ready: /run/systemd/seats)
polkitd            3.447s
udev               3.197s
network           10.505s   (oneshot)
network-manager   11.757s
getty-tty1        10.755s
sshd              10.755s
display-manager   13.760s   ← LightDM login screen visible
```

total kernel → login screen: **~20.7s**

`schema-ctl timing` produces this output.

### PID 1 RSS — every measurement

One table, every number, each naming the machine, the build and the service count it came from. Anything not listed here is not a measurement we have.

| PID 1 | RSS | Machine / conditions | Measured |
|-------|-----|----------------------|----------|
| schema-init | **1.2 MB** | minimal static boot, QEMU/KVM 512 MB / 2 vCPU | 2026-06-14 |
| schema-init | **2.6 MB** | live desktop, QEMU/KVM 512 MB / 2 vCPU | 2026-06-14 |
| schema-init | **3.3–4.0 MB** | Fedora 44, KDE Plasma + Docker/podman, **47 services**, v0.1.0 — six consecutive boots | 2026-07-16 → 07-24 |
| systemd | **20.1 MB** | Fedora Cloud Base 44 clean idle, 15 running units, same kernel, same QEMU profile | 2026-06-14 |

**On the same kernel and QEMU profile that is 8–17× lighter.** The desktop and the Cloud Base figures are *not* a fair pair — one runs KDE, the other is headless — so they are not presented as one. The only apples-to-apples comparison here is minimal-boot schema-init vs clean-idle systemd.

Two numbers this README used to carry, and why they're gone:

- **"892 KB"** was real, but it was an earlier and smaller build on the Dell. Current builds measure 1.2 MB minimal and 3.3–4.0 MB at a full desktop; the init has grown (timers, cpuset, cgroup delegation, container support). Leading with the lowest figure ever recorded, from a binary you can no longer download, isn't a footprint claim — it's cherry-picking. **892 KB was also never the binary's size on disk** (see [Binary size](#binary-size)).
- **"40 MB – 120 MB" for systemd's PID 1** was never measured by this project. The measured figure is 20.1 MB, above.

### Binary size

`make` produces an unstripped static binary. Measured on the v0.1.0 build (`601b18ac`, 2026-07-24):

| | Bytes | |
|---|---|---|
| as built (static, with debug info) | 5,607,840 | **5.6 MB** |
| after `strip schema-init` | 1,199,144 | **1.2 MB** |
| `.text` alone | 1,119,086 | 1.1 MB |

`.text` alone is 1.1 MB, so no build of this binary has ever been under 1 MB on disk. Reproduce with `ls -l`, `size` and `strip`. Note that plain `make` does **not** strip — quote 5.6 MB for what you build yourself, 1.2 MB only for a stripped binary. `make release` produces the stripped set in `release/` alongside a `SHA256SUMS` file; that is what release assets ship.

### Architectural efficiency

Live measurements from a 9-hour uptime session (Fedora 44, KDE Plasma, GreyBox — note this node runs an older, smaller build; its RSS is not comparable to the current one):

| Metric | systemd | schema-init | Architectural elimination |
|--------|---------|-------------|--------------------------|
| Idle CPU consumption | Constant ambient timer wakeups | ~0.03ms/min (1.06s over 9h) | CPU reaches deeper C-states — hardware idle, not just low-utilization idle |
| State tracking | D-Bus event loops, logging daemons | Direct POSIX shared memory / binary flag probes | Removes IPC serialization and deserialization bottlenecks entirely |
| Session tracking | utmp/logind infrastructure | Ghost sessions — `who`/`w` show 0 users | Zero inode contention on `/var/run/utmp`; `who` and `w` are zero-overhead no-ops under concurrent logins |

The load average on an idle system with schema-init as PID 1 sits at 0.03. On the same hardware with systemd, ambient timer wakeups hold it at 0.10–0.20 at idle. The difference is structural: schema-init's tick loop sleeps indefinitely once all services are stable. Nothing wakes it.

`turbostat` on Eli (Dell Inspiron 3542, Intel i3-4005U, Fedora 44, full Cinnamon desktop):

```
C10%: 92–99%    ← deepest available C-state; CPU hardware-verified
C6%:  0.00%     ← skipped; CPU goes straight to C10
Busy: 0.21–0.38%
PkgWatt: 1.23–1.32W   ← entire SoC including iGPU, read via Intel RAPL
GFX%rc6: 99.67%        ← integrated GPU in deepest sleep state
```

C10 is the deepest sleep state on Haswell silicon. Reaching it requires the CPU to sit undisturbed long enough to flush caches and power-gate internal voltage rails — typically blocked by the constant timer wakeups from systemd's watchdog, journal flush, and D-Bus polling infrastructure. At 92–99% C10 residency with a full desktop running, schema-init is generating near-zero ambient noise. The 1.25W package figure is read directly from Intel RAPL hardware energy counters, not estimated. Services with `ready_path` set promote the instant the path exists — no blind timer. `stable_secs` (default 10s) is the fallback. The remaining ~10s cluster is network/getty/sshd with no readiness path.

---

## Runtime control

`schema-ctl` is a control client that communicates with the running init over a Unix domain socket at `/run/schema-init.sock`.

```sh
sudo schema-ctl status          # full state dump for all services
sudo schema-ctl status --json   # machine-parseable JSON — for supervisory loops and IEC 62304 audit
sudo schema-ctl status --kv     # flat key=value — grep-friendly
sudo schema-ctl list            # names and current states only
sudo schema-ctl start <name>    # start a stopped or EXCISED service
sudo schema-ctl stop <name>     # send SIGTERM to a running service
sudo schema-ctl restart <name>  # stop + re-queue through the state machine
sudo schema-ctl add <path>      # load a new .svc file at runtime, no reboot needed
sudo schema-ctl reload          # re-read the services directory (rejected if new config has a cycle)
sudo schema-ctl reload --evict  # reload + SIGTERM any running service no longer present in config
sudo schema-ctl pet <name>      # service heartbeat check-in — resets watchdog_timeout_ms window
sudo schema-ctl reset [<name>]  # reset restart/dormant counts and re-queue failed services
```

The socket is `chmod 0600` — root only. Build alongside the init binary:

```sh
make schema-ctl
sudo cp schema-ctl /usr/local/bin/schema-ctl
```

---

## Recovery console

When a Wayland compositor wedges, `ctrl-alt-F2` only gets you another login on the same broken session, and systemd's `rescue`/`emergency` targets are all-or-nothing — they tear the session down and lose your work. `schema-board` on a dedicated VT is the alternative: a surface that sits **below** the compositor and shows you what is actually wrong.

```sh
schema-board --tty /dev/tty8                    # then ctrl-alt-F8 to look at it
schema-board --tty /dev/tty8 --interactive      # ...and fix something from there
```

`--interactive` adds a cockpit: `↑`/`↓` (or `j`/`k`) to select a service, `enter` to raise a card,
`y` to apply, `n` to cancel. The card is chosen from the service's state — `DORMANT` gets `reset`,
`EXCISED` gets `start`, anything else gets `restart` — and the confirm panel prints the exact
command before it runs:

```
 ▸ restart frigate?
   will run: schema-ctl restart frigate
   [y] apply   [n] cancel
```

**Browsing stays read-only.** The board is a pure shared-memory reader until you press `y`, so it
needs no root to watch and keeps working when the control socket is wedged. Only applying a card
opens the socket, and that needs root. The board can only ever issue a command you could have typed
yourself. `ctrl-C` always works — `ISIG` is left on deliberately.

Note that this lets anyone at the physical console restart a service. That is not a new privilege
boundary — the shipped gettys autologin root on tty2 — but it is worth knowing before you enable it
on a machine other people can walk up to.

It reads the shared-memory export rather than the control socket and depends on nothing graphical, so a frozen desktop, a wedged control socket, and a saturated D-Bus all leave the **process** working. Give it a VT no getty owns — `services/` ships gettys on tty2–tty6, and tty1 is the display manager, so tty7 and up are free.

### What it survives, and what it does not

✅ **VT switching works on a graphical schema-init system.** This was broken until 2026-07-26 and is now fixed in `scripts/schema-logind.py`. Confirmed on real hardware — NVIDIA, `sddm`-started KDE Wayland session — by a human looking at the screen, which is the only evidence that settles a question about what is visible.

| | |
|---|---|
| The board keeps reading and updating while the compositor is wedged | **Yes** — `seq` advanced 228430 → 228569 across a 30 s freeze with `kwin_wayland` in state `T` |
| You can *see* it while the compositor is **healthy** | **Yes** — `ctrl-alt-F8` shows the console, `ctrl-alt-F1` returns to a repainted desktop |
| You can *see* it while the compositor is **wedged** | **Yes** — the board rendered, in colour, with `kwin_wayland` in state `T` |

**The wedged case is what the recovery console exists for, and it is the fail-safe that carries it.** With the compositor `SIGSTOP`ed it cannot answer `PauseDeviceComplete`, so every device ack goes missing. Releasing the VT anyway — rather than waiting for acks that will never arrive — is the only reason the switch completes:

```
VT release requested — pausing 10 device(s)
DROP_MASTER ok on fd=11
10 device ack(s) missing — releasing anyway
VT_RELDISP(1) — switch allowed to proceed
```

Blocking there would strand the kernel mid-switch with DRM master already dropped: a black screen with no way back. On `SIGCONT` the compositor flushes all ten acks and a late `Seat.SwitchTo` for the keypress it was frozen on, and the session recovers. Note that in the wedged case the kernel drives the handoff alone through `VT_PROCESS` — `Seat.SwitchTo` arrives *after* the release, not before it as in the healthy case. That is the difference a polling implementation cannot cover.

### How the handoff works

A graphical session needs this chain, and every link now exists:

```
ctrl-alt-F<n>
  → kernel signals schema-logind (VT_PROCESS) and WAITS   ✅ VT_SETMODE at TakeControl
  → logind sends PauseDevice to the compositor            ✅ pause, per device taken
  → logind drops DRM master                               ✅ DRM_IOCTL_DROP_MASTER
  → logind acks with VT_RELDISP, kernel completes switch   ✅
  → fbcon restores the mode with master already free      ✅ console repaints
  → on return: VT_RELDISP(VT_ACKACQ), SET_MASTER, ResumeDevice
```

**`VT_PROCESS` mediation is the load-bearing part, and a polling implementation cannot replace it.** An earlier fix watched `/sys/class/tty/tty0/active` every 250 ms and implemented `PauseDevice`/`ResumeDevice`, `VTNr`, and `Seat.SwitchTo` — all necessary, none sufficient. The kernel completes a VT switch *synchronously*, and fbcon's mode restore runs during it, while master is still held; it fails silently and is never retried. Measured: after the poll dropped master the console sat at **15** non-black pixels, and a *second* switch — master already free — painted **32,771**. The active-VT poll survives only as a fallback for when `VT_SETMODE` fails.

Two things this depends on, both worth knowing before you touch it:

- **`ReleaseControl` restores `VT_AUTO`.** KWin calls it from `~LogindSession`. Anything that takes control and exits — a display-manager greeter, for instance — tears mediation down for whoever comes next.
- **The chord arrives twice**, once from the kernel's VT handler and once from the compositor calling `Seat.SwitchTo` for the same keypress. Handling it twice overwrites and leaks the pending-ack timer.

Two earlier revisions of this section were wrong in ways worth recording. The first blamed a *stopped* compositor for being unable to release DRM master; a healthy one does not release it either, because nothing asks. The second proposed having the board take DRM master itself, which cannot work — `DRM_IOCTL_SET_MASTER` fails while another process holds master. The real cause was that `Properties.Get` for `VTNr` failed, so KWin's `LogindSession::create()` bailed and it silently fell back to `NoopSession`, whose `switchTo()` is an empty function body.

**On a machine with no graphical session** — a server, a Pi, an initramfs-less boot before the display manager starts — the recovery console works regardless, because nothing has taken KMS.

If you are stranded on an invisible VT, `sudo chvt 1` from any other shell (ssh included) puts you back.

To have PID 1 own it from boot, copy `services/schema-board.svc.example` into `/etc/schema-init/services/`:

```ini
name=schema-board
exec=/usr/bin/schema-board
args=--tty
args=/dev/tty8
needs_root=1
critical=0
```

Two things about that file are load-bearing:

- **`args=` is one argument per line.** `args=--tty /dev/tty8` on a single line passes *one* argv of `"--tty /dev/tty8"`, which `schema-board` rejects. Repeat the key.
- **`--tty` is not optional for a service.** Services are spawned with stdout redirected to `/var/log/schema-init/<name>.log`, so without `--tty` the board would faithfully paint its frames into a logfile.

On the console it takes over, the board disables screen blanking and hides the cursor, restoring the cursor when it exits.

---

## Debugging

### Service state

```sh
sudo schema-ctl status          # full dump: state, pid, restart count, weight
sudo schema-ctl list            # compact: name + state only
sudo schema-ctl timing          # kernel→PID1 handoff + per-service stable timestamps
```

A service stuck in `NEW_PROCESS` means its dependencies haven't stabilised. `status` shows the state of every dep — trace upward.

A service in `FRICTION` is in last-chance recovery. On the next failed F6 probe it enters `DORMANT` (exponential backoff) rather than going straight to EXCISED. Use `sudo schema-ctl start <name>` to manually re-queue it immediately instead of waiting out the backoff.

### Service logs

```sh
tail -f /run/log/schema-init/<name>.log    # live stdout/stderr for a service
cat /run/log/schema-init/dbus.log          # full output since last boot
```

These are plain text on a tmpfs. If a service is failing silently, its output is here.

### D-Bus tracing

If a desktop application hangs for exactly 25–30 seconds, D-Bus auto-activation is timing out trying to reach an unregistered interface. Trace it:

```sh
dbus-monitor --system 2>&1 | grep -A4 "method call"
```

The culprit will appear as a `method call` to a `destination=org.freedesktop.SomeName` that produces no `method return` for ~25 seconds.

Fix options:
1. Register the interface — see schema-logind for the pattern
2. Mask the activation file: `sudo rm /usr/share/dbus-1/system-services/<name>.service`

### Rescue shell

If schema-init drops to a rescue shell at boot (cycle detected, or fatal probe failure), you have a minimal `/bin/sh` with access to the mounted filesystems. From there:

```sh
# inspect service files
ls /etc/schema-init/services/
cat /etc/schema-init/services/broken.svc

# fix and re-exec
vi /etc/schema-init/services/broken.svc
exec /sbin/schema-init
```

---

## Logs

**Init log** — schema-init writes spawn/promote/death events to stdout, which the kernel connects to the console at boot. To persist:

```sh
exec /sbin/schema-init >/var/log/schema-init.log 2>&1
```

**Per-service logs** — each service's stdout and stderr are captured automatically to:

```
/var/log/schema-init/<name>.log        # preferred (persists across boots)
/run/log/schema-init/<name>.log        # fallback when /var is not writable (tmpfs, per-boot)
```

To read them while the system is running:

```sh
tail -f /var/log/schema-init/dbus.log
tail -f /var/log/schema-init/network-manager.log
```

There is no journal daemon. Logs are plain text, always.

**Rotation** — `schema-init.logrotate` is installed to `/etc/logrotate.d/schema-init` (daily, or sooner at 100 MB, keeping 4 compressed generations). It covers `/var/log/schema-init/*.log` and the KDE deploy's `/var/log/sddm-schema.log`, which `sddm-logged` writes under `set -x`. It uses **`copytruncate`, and that is not optional**:

- A service's log fd is opened in the child before `exec` (`service.c:321`) and held for the whole life of the process. Renaming the file would leave every running service appending to the old inode — the new file would stay empty until the service restarted.
- `SIGHUP` to PID 1 means **reload configuration** (`init.c:1312`), not "reopen logs". A `postrotate kill -HUP 1` would silently trigger a config reload instead of rotating.
- Every writer uses `O_APPEND` (`service.c:321`, `schema-journal-sink.c:344`), which is what makes truncation safe: writes resume at offset 0 instead of leaving a sparse hole at the old offset.

**Something has to run logrotate.** There is no cron and no `logrotate.timer` here, so schedule it as an ordinary wall-clock timer — see `services/logrotate.svc.example`:

```ini
name=logrotate
exec=/usr/sbin/logrotate
args=/etc/logrotate.conf
needs_root=1
on_calendar=00:10
persistent=1
```

Copy it into your service directory to arm it:

```bash
sudo cp /usr/share/schema-init/services/logrotate.svc.example \
        /etc/schema-init/services/logrotate.svc
sudo schema-ctl reload
```

`persistent=1` matters here: a box that is powered off at 00:10 would otherwise skip that day's rotation entirely and only catch up the next night.

Without that timer (or some other caller) the config sits inert and logs still grow unbounded. `maxsize 100M` is not a safety net on its own — it is only consulted when logrotate actually runs.

**`journalctl` shim (optional Track B)** — software and post-install scripts that shell out to `journalctl -u <svc>` would fail with no journald present. `scripts/journalctl` is a drop-in interceptor: install it to `/usr/local/bin/journalctl` and it serves the matching `*.log` from the directories above, swallows unknown flags, supports `-o json`, and always exits 0 so a caller piping it to `jq`/`awk` never hard-crashes. It does **not** read a binary journal — there isn't one.

---

## Shared memory interface

Running processes can read service state via POSIX shared memory at `/schema-init`:

```c
#include "schema_shm.h"

int fd = shm_open("/schema-init", O_RDONLY, 0);
schema_shm_t *shm = mmap(NULL, sizeof(schema_shm_t), PROT_READ, MAP_SHARED, fd, 0);

for (int i = 0; i < shm->count; i++) {
    printf("%s state=%d weight=%d pid=%d\n",
           shm->svc[i].name,
           shm->svc[i].state,
           shm->svc[i].weight,
           shm->svc[i].child_pid);
}
```

---

## D-Bus compatibility

On a no-systemd desktop, several interfaces are missing that desktop environments expect. schema-logind (`distros/*/services/schema-logind.svc`) handles the session/power/host interfaces in a single Python process on the system bus. The `org.freedesktop.systemd1` management surface is served by its own process — see below.

| Interface | Why it matters | What schema-logind returns |
|-----------|---------------|---------------------------|
| `org.freedesktop.login1` | Power/reboot buttons, session tracking, polkit seat queries | PowerOff, Reboot, CanPowerOff, CanReboot, Inhibit, GetSessionByPID, mock Session/User/Seat objects |
| `org.freedesktop.ConsoleKit` | Cinnamon session manager uses ConsoleKit, not logind, for CanRestart/CanStop — controls restart button visibility | GetSessionForUnixProcess, CanRestart → True, CanStop → True, Restart/Stop → SIGINT/SIGTERM to PID 1 |
| `org.freedesktop.hostname1` | About This System panel, network-manager display | hostname, static hostname, OS pretty name, hardware vendor/model from `/sys/class/dmi/` |
| `org.freedesktop.systemd1` | `systemctl`, Cockpit's Services page, and KDE/GNOME unit-state queries — the full systemd-compat management surface | **Live** per-unit ActiveState/SubState/MainPID/NRestarts mapped from `schema-ctl`; ListUnits/ListUnitsFiltered, GetUnit, GetUnitFileState, StartUnit/StopUnit/RestartUnit driving `schema-ctl` for real; PropertiesChanged on state transitions. Served by a **separate** `schema-systemd1` process (see below) |
| `org.freedesktop.timedate1` | Date & Time settings panel: timezone, NTP status, clock | Timezone (from `/etc/localtime`), CanNTP/NTP/NTPSynchronized → true, TimeUSec; `SetTimezone` re-links `/etc/localtime` and writes `/etc/timezone` for real |

Without these stubs, KDE and GNOME panels hit the D-Bus default timeout (25–30s) before giving up. With them, the same queries return in <100ms.

**D-Bus policy required.** The systemd-shipped `org.freedesktop.login1.conf` policy denies all non-root calls to login1 by default — KDE and Cinnamon will never see the power buttons without a drop-in. Install the one from this repo:

```sh
sudo cp distros/shared/dbus/schema-logind.conf /etc/dbus-1/system.d/schema-logind.conf
sudo dbus-send --system --type=method_call --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.ReloadConfig
```

Then log out and back in (or reboot). The policy whitelists CanPowerOff, CanReboot, PowerOff, Reboot, and all session/seat methods schema-logind exports.

**schema-logind is not a dependency of schema-init itself** — it is a userspace service like any other. Drop its `.svc` file in your services directory and list it as a dep of your display manager:

```ini
name=sddm
exec=/usr/sbin/sddm
dep=dbus
dep=schema-logind
dep=polkitd
needs_root=1
```

**The real systemd1 surface (`schema-systemd1`).** Unlike the read-only stubs above, `org.freedesktop.systemd1` is served by its own process (`services/schema-systemd1.svc` → `scripts/schema-systemd1.py`), not schema-logind. It registers every schema-init unit as a `…/unit/<name>_2eservice` object and mirrors live state from `schema-ctl`, so `systemctl status <svc>` and Cockpit's Services page show real ActiveState/SubState/MainPID/restart counts — and `StartUnit`/`StopUnit`/`RestartUnit` drive `schema-ctl` for real. Unit names are validated before being handed to `schema-ctl` (argv/newline injection guard). State transitions emit `PropertiesChanged`, so Cockpit updates without a refresh. Design notes: `docs/superpowers/specs/2026-06-20-schema-systemd1-dbus-design.md`.

**The `sd_booted()` signal.** `mount_pseudo()` creates `/run/systemd/system` at early boot (`init.c`). `libsystemd`'s `sd_booted()` is a bare `access()` on that path, so any software gated on "is systemd the init?" — KService/ksycoca, elogind clients — gets a positive answer with no shim. This is what made the old `LD_PRELOAD` `mock_sd.so` workaround (which faked the check to stop KDE's ksycoca from spinning at idle) unnecessary: the signal is now native and costs one `mkdir`.

**The `sd_login_monitor` directories.** `libsystemd`'s `sd_login_monitor_new(NULL, …)` — used by WirePlumber's logind module and other session/seat-aware clients — sets an inotify watch on `/run/systemd/{sessions,seats,users,machines}`. If any of those directories is missing the call fails with `-ENOENT` and the client silently drops logind integration (for WirePlumber that means no device reservation, no session-based pause). schema-logind creates all four at startup, empty — matching what real logind does even with no active sessions — so those clients initialize cleanly. Costs four `mkdir`s.

---

## Porting to a new distro

Starting from scratch on a distro not in `distros/`:

**1. Build the binary on the target (or cross-compile):**
```sh
git clone https://github.com/ajax80/schema-init
cd schema-init && make
```

**2. Install:**
```sh
sudo cp schema-init /sbin/schema-init
sudo cp schema-ctl  /usr/local/bin/schema-ctl
sudo mkdir -p /etc/schema-init/services
```

**3. Write service files.** Start minimal — just enough to reach a console:

```ini
# /etc/schema-init/services/udevd.svc
name=udevd
exec=/usr/lib/systemd/udevd
args=--daemon
needs_root=1
stable_secs=3

# /etc/schema-init/services/dbus.svc
name=dbus
exec=/usr/bin/dbus-daemon
args=--system
args=--nofork
needs_root=1
stable_secs=2
ready_path=/run/dbus/system_bus_socket
```

The udevd path varies by distro: `/usr/lib/systemd/udevd` (Fedora/Debian), `/lib/udev/udevd` (older Debian), `/usr/bin/udevd` (Arch).

**4. Configure GRUB** (see Building → GRUB setup above). Boot with a fallback entry pointing at systemd so you can recover.

**5. Boot and check:**
```sh
sudo schema-ctl list       # all services should reach FUNDAMENTAL
sudo schema-ctl timing     # see where time goes
tail /run/log/schema-init/udevd.log   # if something is EXCISED, check its log
```

**6. Add services incrementally.** Bring up network, then login manager, then display manager. Add `dep=` links to enforce order. Add `ready_path=` for anything with a socket or pidfile.

**7. Handle D-Bus hangs.** Open your desktop's settings panel immediately after first login. If it hangs >5s, run `dbus-monitor --system` and identify the missing interface. Add a stub to schema-logind or mask the activation file.

**Common issues by distro:**

| Issue | Cause | Fix |
|-------|-------|-----|
| udevd not populating /dev/input | udev not settled before display manager | `dep=udev` in display manager svc; `udevadm settle` in a oneshot before it |
| polkit "not authorized" on NM | polkit rule missing wheel group | Copy `distros/fedora-kde/config/polkit/10-schema-nm.rules` |
| `/etc/resolv.conf` is a dead symlink | systemd-resolved wrote it | `rm /etc/resolv.conf && echo "nameserver 1.1.1.1" > /etc/resolv.conf` in your network oneshot |
| Plasma/GNOME hangs on settings open | Missing D-Bus interface | See D-Bus compatibility section above |
| PipeWire/PulseAudio not starting | systemd user session missing | Add autostart `.desktop` entry, or run from display manager wrapper script |
| display manager exits immediately | No seat available | Ensure elogind or schema-logind is up and answering login1 before display manager starts |
| X11/XWayland apps die with `Unable to open display` (Steam, any non-Wayland-native app) | `systemd-tmpfiles` normally creates `/tmp/.X11-unix` as `1777 root:root`; with no systemd it's missing or wrong-perm, and **an X server refuses a `/tmp/.X11-unix` without the sticky bit** — so the compositor's XWayland silently never starts and `DISPLAY` is never exported | oneshot before the display manager: `mkdir -p /tmp/.X11-unix && chown root:root /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix` (also clear stale `/tmp/.X[0-9]*-lock`). `dep=` it from the DM. On a root-fs `/tmp` (not tmpfs) the broken dir persists across reboots, so this isn't self-healing |
| flatpak/snap apps won't launch — `The name org.<app>.desktop was not provided by any .service files` | The **session** D-Bus bus computes its `.service` search dirs once at startup from `XDG_DATA_DIRS`; with no systemd user env-generator that variable is unset when the bus is born, so it never scans `…/flatpak/exports/share/dbus-1/services`. Anything later (a `plasma-workspace/env` script) runs *inside* the bus's child — too late | Export `XDG_DATA_DIRS=$HOME/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:/usr/local/share:/usr/share:/var/lib/snapd/desktop` in the env that launches the session bus, **before** the bus starts. Stopgap without re-login: symlink the `*.service` files into `~/.local/share/dbus-1/services/` (always searched regardless of `XDG_DATA_DIRS`) and `ReloadConfig` the bus |

---

## Distributions

Working configurations for specific distros and desktops live in `distros/`.

### Fedora 44 + KDE Plasma (`distros/fedora-kde/`)

Full KDE Plasma 6 desktop on Fedora 44 with schema-init as PID 1. Boots from a btrfs subvolume alongside a normal Fedora install — no repartitioning required.

**What's running:**

| Service | Role |
|---------|------|
| `udevd` | Device enumeration — required for libinput and /dev/input/event* |
| `dbus` | System bus |
| `network-up` | Loads r8152 USB ethernet module, udev settle |
| `network-manager` | Owns the network interface via NM profile |
| `polkitd` | Authorization — required for NM actions |
| `schema-logind` | Minimal `org.freedesktop.login1` D-Bus stub — restores KDE shutdown/restart buttons |
| `sddm` | Display manager (via sddm-logged wrapper, no systemd session) |
| `sound-modules` | oneshot — loads AMD Ryzen audio modules at boot |
| `bluetoothd` | Starts `bluez` daemon — registers `org.bluez`, restores KDE Bluetooth applet |
| `zram-swap` | oneshot — zstd-compressed zram swap device; replaces systemd's `zram-generator` |

See [`distros/fedora-kde/README.md`](distros/fedora-kde/README.md) for full installation instructions and key fixes.

### Raspberry Pi Zero W (`distros/raspberry-pi-zero-w/`)

WiFi headless deploy on a Pi Zero W (BCM2835, armv6l, 32-bit ARM). No Ethernet, no HDMI — schema-init as PID 1, WiFi up, SSH accessible in ~50 seconds from cold boot. First ARM bare-metal target.

**Service chain:**

| Service | Role |
|---------|------|
| `udev` | Device enumeration daemon |
| `udev-trigger` | Oneshot — coldplug trigger + settle; loads brcmfmac WiFi firmware |
| `dbus` | System bus — mandatory for Pi OS wpa_supplicant |
| `wpa-supplicant` | WiFi association (config-file mode, not D-Bus mode) |
| `dhcpcd` | DHCP client, foreground (`-B`), wlan0 only |
| `sshd` | First usable interface — up when DHCP lease is held |

See [`distros/raspberry-pi-zero-w/README.md`](distros/raspberry-pi-zero-w/README.md) for the full list of gotchas (rfkill country code, dbus privilege drop, coldplug trigger, dhcpcd forking behavior) and installation steps.

---

## Roadmap

- [x] Runtime service loading — `schema-ctl add <path>` loads a new service at runtime
- [x] Runtime reload + removal — `schema-ctl reload [--evict]` re-reads config (cycle-checked); `--evict` SIGTERMs services dropped from config, no reboot
- [x] login1 D-Bus stub — `schema-logind` restores KDE shutdown/restart buttons on no-systemd systems
- [x] event-driven main loop — signalfd for SIGCHLD + poll() with 250ms timeout; wakes on child death and ctl commands instead of busy-polling
- [x] Boot hang fix — dep_idx alignment bug in group dep resolution; poll() replaces epoll (PID 1 epoll deadlock on kernel 6.1.0-49)
- [x] Boot timing — `schema-ctl timing` reports kernel→PID1 handoff and per-service FUNDAMENTAL/PERFECT timestamps (CLOCK_MONOTONIC)
- [x] Boot time measurement — 29.5s → 20.7s with `ready_path` probes; `stable_secs` fallback per service
- [x] Per-service readiness probes — `ready_path=` promotes on path existence; `stable_secs=` fallback timer
- [x] Cgroup assignment race fix — pipe barrier guarantees cgroup.procs written before child exec
- [x] Dynamic poll timeout — loop sleeps indefinitely once all services stable; 0% CPU idle
- [x] Service log files — stdout/stderr per service at `/run/log/schema-init/<name>.log`
- [x] D-Bus stubs — `hostname1` and `systemd1` Manager stubs in `schema-logind`; KDE Settings 25s → 2s
- [x] Fedora KDE distribution — GreyBox daily driver, full KDE Plasma 6 on Fedora 44
- [x] Fedora Cinnamon distribution — Eli (Dell Inspiron), keyboard/touchpad/ethernet working
- [x] STATE_DORMANT (75) — exponential backoff before 76 verdict; critical services never excise
- [x] Soft dep cascades — non-critical EXCISED deps skipped; dependents proceed without them
- [x] aarch64 cross-compile — `make aarch64`; all three binaries static; Ungulate Leg target ready
- [x] ARM bare-metal deploy — Pi Zero W (armv6l), Pi OS Trixie; SSH up in ~50s from cold boot
- [x] schema-desktop — SDL2 live service viewer; `make desktop` + autostart entry in Cinnamon and KDE distros
- [x] Dead Man Token hardware watchdog — `/dev/watchdog` driven by per-service check-in via `schema-ctl pet`; any critical service missing its `watchdog_timeout_ms` window stops WDT petting → hardware reboot; PID 1 deadlock covered implicitly
- [x] Symlink template instances — `motor@12.svc → motor@.svc`; `$INSTANCE` injected at spawn; `$SLOT_ID` fallback for GPIO-strapped nodes; one SD card image per fleet
- [x] Structured telemetry — `schema-ctl status --json` and `--kv` for machine-parseable supervisory loop consumption and IEC 62304 audit traceability
- [x] Cgroup resource limits — `cpu_limit=` (1–100, % of one core), `mem_limit=` (MB), `cpuset=` (CPU affinity / core pinning, systemd `AllowedCPUs=` analog), and `cpuset_partition=` (`isolated`/`root` exclusive cores via cgroupv2 partitions — dynamic `isolcpus`) per `.svc`; written via sync-pipe window before child exec; IEC 62304 Class C blast-radius isolation
- [x] zram swap — `zram-swap.svc` boots a zstd-compressed zram swap device, replacing systemd's `zram-generator`; eliminates disk thrashing / periodic stutter under memory pressure

---

## Origin

schema-init was built by Jonathan Ayers in 2026 as the operating layer for the **Ungulate Leg** — an exoskeleton controller that needs a deterministic, schema-driven PID 1 carrying none of systemd's assumptions about what a computer is for. (A sister project, the **Samara Wing**, applies the same architecture to flight.) The reference hardware that proved it — a pawn-shop Dell Inspiron named Eli — booted to a full desktop under schema-init on May 30th, 2026. It was written alongside Claire, an AI (Claude, by Anthropic) that held continuity across the work.

---

## License

**AGPL-3.0-or-later** for open source use — full text in [LICENSE](LICENSE).  
Commercial license available for embedding in proprietary systems — open an issue or contact via GitHub.

---

*Built by Jonathan Ayers. The schema was written before the code.*
