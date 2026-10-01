# Idle PID 1: sleep on events and deadlines, not a 250 ms tick

**Status:** Draft 2026-09-30, Greg review folded in (DORMANT deadline, past-deadline → 0, inotify notes). Awaiting Jonathan's approval.
**Component:** `init.c` main loop (`get_poll_timeout`, `tick_service` STATE_FUNDAMENTAL,
`watchdog_pet`, the survival block), `service.h` (pure helper + tests).

## Problem

PID 1 wakes 4×/s on every host, forever. Measured 2026-09-30 18:5x, `voluntary_ctxt_switches`
of PID 1 over 10 s on a settled system:

| host | wakes / 10 s | ready_path services |
|---|---|---|
| blakbox | 40 | journal-sink, dbus, udevd, pipewire, schema-logind, docker, containerd |
| DBox | 40 | journal-sink, dbus |
| Eli | 42 | journal-sink |
| Optiplex | 40 | journal-sink, udevd, seatd, docker, containerd |

Cause: `get_poll_timeout()` (`init.c:1507`) returns the 250 ms tick while any
`STATE_FUNDAMENTAL` service with `ready_path=` has a live child, because the
readiness-lost check in `tick_service` (`init.c:931`) is a polled `access()`. Every
host runs `journal-sink.svc` with `ready_path=/run/systemd/journal/socket`, so no host
ever goes idle. On DBox and Eli (laptops) that is a steady wakeup source that keeps the
CPU out of deep package C-states.

### Latent bugs the tick is hiding

The tick has masked several places where the loop needs to wake up but
`get_poll_timeout()` doesn't ask for it. They work today only because
journal-sink keeps the loop ticking. With the tick gone they would break silently:

1. **Timer services never fire.** Timers sit in `STATE_PERFECT`, and `get_poll_timeout()`
   treats PERFECT as settled. `timer_next` is only compared inside `tick_service`. With no
   ready_path service, `poll()` gets `-1` and `cron-daily`, `logrotate`, `kodi-autolib`,
   `schema-doctor-periodic` etc. sleep until an unrelated signal arrives.
2. **Eviction grace deadlines** (`eviction_tick`) are not counted. An orphan that ignores
   SIGTERM is not force-killed on time.
3. **Failsafe timeouts** (`monitor_failsafes`, default 500 ms) are not counted either.
4. **avg10 fallback** (`psi_fd < 0`, e.g. `psi=0` kernels) only enters survival mode when a
   pass happens to run.
5. **Critical-cgroup CPU pressure** (`critical_cpu_pressure`, avg10 > 5 on
   `priority=critical` services) is only read on a pass.

Each of these must become an explicit wake reason before the ready_path tick can go.

## Goal

A settled host wakes PID 1 only for (a) real events on its fds and (b) the nearest real
deadline. The target is to go from 4/s to the hardware-watchdog pet rate, plus the rare timer fire.

Non-goals: changing readiness *acquisition* (NEW_PROCESS→FUNDAMENTAL still polls at the tick
while services are transitional), boot speed, notify/bus readiness.

## Design

### 1. `next_wake_ms()` replaces the boolean tick

`get_poll_timeout()` becomes "time until the nearest deadline, clamped to the tick":

**Tick (250 ms), as today, while any of:**
- `system_under_pressure` (survival posture active; thaw needs 5 s silence timing)
- any service in a non-settled state (anything but FUNDAMENTAL / PERFECT / EXCISED /
  DORMANT)
- any software-watchdog service live (`watchdog_timeout_ms > 0`)
- any `failsafe_pid > 0` (short-lived; keeps the 500 ms failsafe timeout exact)
- `psi_fd < 0` (avg10 fallback: unchanged behaviour on psi-less kernels)
- inotify unavailable (`ino_fd < 0`) and any verified ready_path service live, which falls
  back to today's polling

**Otherwise, min over these deadlines** (`-1` if none):
- each PERFECT timer's `timer_next`:
  - monotonic timers exact
  - calendar timers converted from CLOCK_REALTIME and **capped at 60 s**, so an NTP step
    or an RTC-less Pi syncing its clock can't oversleep a calendar fire
- each pending eviction deadline
- each DORMANT service's `dormant_until` (300–3600 s backoff, `init.c:963`). Without this,
  one backed-off service would pin the tick for up to an hour (Greg)
- ready_path backstop: `READY_BACKSTOP_MS` = 30 000 (see §2)
- hardware watchdog pet: `wd_pet_ms` (see §3)
- critical CPU pressure check: 2 000 ms while any `priority=critical` service is live (see §4)

A deadline at or before now returns **0** (non-blocking pass), never a negative or
wrapped value: `poll(-1)` would block forever (Greg). The computation is in signed
64-bit ms.

The arithmetic goes in a pure function in `service.h` (the same pattern as `pressure_step()`):
it takes the deadline inputs and "now" values and returns ms. Tested in `tests/test_next_wake.c`
with no fds and no clocks.

### 2. ready_path liveness via inotify

- One `ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC)` at startup. Add it to the poll set
  (`fds[]` grows 4 → 5).
- When a FUNDAMENTAL service's `ready_path_verified` flips to 1, call
  `inotify_add_watch(ino_fd, dirname(ready_path), IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)`,
  then **re-`access()` once** to close the check-then-watch race.
- On any readable `ino_fd`: drain it, then **100 ms later** (`READY_EVENT_GRACE_MS`) run
  `ready_recheck()` on **every** verified ready_path service. The grace is there because a
  crashing service often unlinks its own socket just before it exits. Without it, the
  inotify event beats SIGCHLD and a plain crash gets logged `readiness-lost` and parked
  DORMANT instead of taking the normal restart path. The 250 ms poll had the same race in
  a narrow window; inotify would make it the common case. (Found while implementing.) No per-name matching, no wd refcounts. Services sharing a directory
  share a watch; the kernel dedups it and returns the same wd. A spurious event costs a
  handful of `access()` calls. Stale watches (service died) are harmless for the same reason.
  The set of watched dirs is bounded by config.
- `IN_Q_OVERFLOW` (wd −1) and `IN_IGNORED` (a watch dropped because its dir was deleted or
  unmounted) need no special handling. They are just more events, so they trigger the same
  full recheck, silently. A recreated dir isn't re-watched until its service re-verifies
  (respawn → FUNDAMENTAL) or SIGHUP. The backstop covers the gap.
- `ready_recheck(svc)` is the existing readiness-lost body lifted out of `tick_service`
  unchanged: log `readiness-lost`, kill, failsafe, dormant/excise backoff.
- **Backstop:** `ready_recheck` also runs every `READY_BACKSTOP_MS`. That covers the cases
  inotify can't see: a path on a filesystem that doesn't send events, a dir replaced by a
  bind mount. `ready_poll_hz=` keeps its meaning as an explicit per-service polling rate:
  a service that sets it keeps the tick and its old counter, so `ready_poll_hz=4`
  reproduces today exactly. If `inotify_add_watch` fails (`ready_watched = -1`), that
  service also falls back to tick polling.
- SIGHUP config reload: `ino_fd` is kept. Reloaded entries start with `ready_watched = 0`
  and re-add their watch on the next pass. `inotify_add_watch` on an already-watched dir is
  idempotent, so nothing gets recreated.
- Readiness-lost latency goes from 0–250 ms (polled) to a fixed ~100 ms (the grace).

### 3. Hardware watchdog pet interval from the device

Every host has one open (blakbox SP5100 60 s, DBox wdat 30 s, Eli/Optiplex intel_oc 60 s).
Today it's petted every loop pass, at least every 5 s. At `watchdog_init` read
`WDIOC_GETTIMEOUT`: `wd_pet_ms = timeout * 1000 / 3`, floor 1 000, and 5 000 if the ioctl
fails. The pet stays gated on the software-watchdog check exactly as now. A PID 1 lockup
still stops the pets and the chip still resets the box. Only the margin moves, from 5 s to
timeout/3 (10 s on DBox, 20 s elsewhere).

### 4. Critical CPU pressure at the avg10 update rate

`read_cpu_pressure()` reads `avg10`, which the kernel updates every 2 s. Reading it at 4 Hz
returns the same number 8 times. Check it on a 2 s deadline (only while a critical service
is live): same data, worst-case detection +2 s. DBox and Eli have no `priority=critical`
services, so this costs them nothing. blakbox (8) and Optiplex (seatd) pay 0.5 wakes/s.
**Deferred option:** per-critical-cgroup PSI triggers on `cpu.pressure` (event-driven, like
#218). Not in this spec. The re-arm on every respawn isn't worth it until the numbers say so.

## Expected result (settled system)

| host | today | after |
|---|---|---|
| DBox | 4 /s | ~0.1 /s (wd 10 s) + timer fires |
| Eli | 4 /s | ~0.05 /s (wd 20 s) + timer fires |
| blakbox | 4 /s | ~0.5 /s (critical CPU check) |
| Optiplex | 4 /s | ~0.5 /s (seatd critical) |

The ready_path backstop (30 s) adds 0.03/s everywhere.

## Commit order (each TDD, `make test` green)

1. **Deadlines first, tick unchanged:** `next_wake_ms()` plus the tests. Wire in timers,
   evictions, failsafes and the psi-fallback tick. This fixes latent bugs 1–4 on its own
   and changes nothing observable while the ready_path tick remains.
2. **inotify ready_path + backstop:** drop the ready_path clause from the tick list.
3. **Watchdog pet from `WDIOC_GETTIMEOUT`**, and the critical-CPU 2 s deadline.

## Verification

- **Unit:** `tests/test_next_wake.c` covers every clamp and min branch, including the
  calendar 60 s cap, past deadline → 0, DORMANT deadline, an empty deadline set (→ `-1`) and a tick-forcing condition.
- **vmtest (schema-vmtest skill), PID 1 under QEMU:**
  - settled wakes/60 s via `voluntary_ctxt_switches` (expect ≤15 vs ~240)
  - `on_active_sec=20` timer fires on time in an otherwise-idle VM (the regression for
    latent bug 1)
  - `rm` a ready_path socket, and the service is killed within ~150 ms; a service that unlinks then exits takes the crash path, not readiness-lost
  - an eviction force-kills on its deadline
  - SIGHUP reload keeps the watches alive (rm after reload still detected)
- **Hardware:** reboot-deploy blakbox (PID 1 → reboot only, never restart). Re-measure
  wakes/10 s, then check that `cron-daily` / `schema-doctor-periodic` fired at their next
  scheduled time in rail.log. Then DBox/Eli/Optiplex. On DBox, compare
  `powertop --csv` PID 1 wakeups and package C-state residency before and after.

## Risks

- **A deadline I missed** is exactly the class of bug this spec exists to expose. Mitigation:
  commit 1 lands with the tick still on, and vmtest's idle-timer test is the canary. Any
  other `tick_service` state check that compares against a clock gets audited in commit 1:
  `grep clock_gettime init.c`, each hit either feeds `next_wake_ms` or runs only in a
  non-settled state.
- **Suspend:** not a fleet concern (Suspend/Hibernate disabled by design). Timers already
  run on CLOCK_MONOTONIC and this spec doesn't change which clock anything uses.
- **Stale ready_path semantics:** `access()` on a socket path only proves the inode exists,
  the same as today. inotify doesn't change what "ready" means.
