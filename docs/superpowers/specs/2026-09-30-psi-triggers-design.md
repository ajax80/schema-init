# PSI Triggers for the Survival Executive

**Status:** Draft — for Jonathan's review. No code yet.
**Component:** `init.c` survival executive (`check_system_pressure`, `execute_survival_posture`, main loop).

## Problem

The survival executive (freeze peripheral, throttle standard, reclaim non-critical)
decides memory pressure from `some avg10` in `/proc/pressure/memory`, read once per
main-loop pass, entering above 10.0 and leaving after 8 clean passes.

`avg10` is a 10-second moving average that the kernel recomputes every 2 seconds,
so it lags a sudden stall on both edges. Measured on blakbox 2026-09-30, contained
scratch cgroup (`memory.high=180M`, `memory.max=900M`, `swap.max=0`, 800 MiB
anon hog for 30 s):

| edge | today (avg10) | kernel trigger `some 150000 1000000` |
|---|---|---|
| pressure starts → react | **5.85 s** (live PID 1 froze at the same second) | **0.69 s** |
| pressure ends → thaw | **~14 s** (hog ended ~17:03:58, thaw 17:04:12) | — (see Exit) |

The trigger fired 30 times over the 30-second hog, once per 1 s window.

A premise from the 09-29 review is wrong for this fleet: "idle PID 1 only checks
pressure when woken". All four hosts wake PID 1 4×/s, permanently
(`voluntary_ctxt_switches` delta, 09-30), because every host runs `journal-sink.svc`
with `ready_path=`, and `get_poll_timeout()` keeps the 250 ms tick while any
`ready_path` service is up (liveness re-check, `init.c:912`). So polling frequency
is not the gap. The average is. Polling faster cannot fix it.

## Design

### 1. Arm one system-wide memory trigger at startup

After `notify_open()`: open `/proc/pressure/memory` `O_RDWR|O_NONBLOCK|O_CLOEXEC`,
write `"some 150000 1000000"` (NUL included). Success → global `psi_fd`, added
to the main `poll()` set with `POLLPRI` (`fds[3]` → `fds[4]`).

Any failure (no `CONFIG_PSI`, `psi=0`, older kernel, `EINVAL`) → `psi_fd = -1`
and the executive runs exactly as today. One boot-log line says which mode is active.

`psi_fd` is a global, not a `service_t` field, so the `handle_reload`
live→shadow merge (the #78 `cgroup_path` class of bug) does not touch it.

### 2. Entry: trigger event

`POLLPRI` (or `POLLERR`, see 5) on `psi_fd` sets `psi_fired = 1` for this pass.
Pressure for the pass = `psi_fired || check_system_pressure()`. The avg10 path
stays as a second source and as the only source when `psi_fd < 0`.
Per-critical-cgroup `cpu.pressure` polling is unchanged.

### 3. Reclaim must not re-check avg10

`execute_survival_posture(1)` only runs `reclaim_pass()` when
`read_system_mem_pressure() > 10.0`. At trigger time avg10 is still ~0, so the
fast entry would freeze but never reclaim. Pass the reason in:
`execute_survival_posture(1, mem_pressure)`, where `mem_pressure` =
`psi_fired || avg10 > 10.0`. Rate-limit `reclaim_pass()` to one per 10 s
(`CLOCK_MONOTONIC`), since faster entry/exit cycles would otherwise fork one per cycle.

### 4. Exit: hysteresis counts trigger silence

Today's rule (8 clean passes ≈ 2 s) stays, but a pass is clean only when there
was no trigger event **and** avg10 ≤ 10. While the stall continues, the trigger
fires every 1 s window and keeps resetting the counter. Once stall time falls
under 150 ms/s the events stop, and the posture clears once avg10 has also
dropped. A falling average should thaw in seconds rather than ~14 s, but that
is untested (open question 2).

Extract the state step as a pure function, e.g.
`int pressure_step(int *under, int *clean, int fired, double avg10)` → returns
enter / exit / none. Unit-test it with no I/O: enter on fired; enter on avg10;
no exit while fired within 8 passes; exit after 8 silent low passes; the psi
fallback case behaves exactly like today.

### 5. Event handling details

- No drain: the kernel raises `POLLPRI` at most once per window and there is
  nothing to `read()` on a trigger fd.
- `POLLERR` means the fd is dead (the kernel tears triggers down on some
  errors): close it, `psi_fd = -1`, log once, fall back to avg10. No re-arm loop.
- The trigger needs no extra wake: PID 1 already sleeps in `poll()` on it, so
  the pass runs within the same scheduler tick as the event.

## Out of scope (named, not built)

- **Idle PID 1.** Replacing the `ready_path` liveness tick with inotify
  (`IN_DELETE`/`IN_MOVED_FROM` on the parent dir) would let PID 1 sleep to the
  5 s hardware-watchdog pet. That saves ~4 wakes/s on the battery boxes (DBox, Eli).
  It needs its own spec, and this design does not depend on it.
- Per-cgroup `cpu.pressure` triggers for critical services. They need fd
  lifecycle on spawn, stop and reload. The polled path works today.
- Tunable thresholds in `.svc`/config. These are constants until a host needs
  something else.
- The user-session hogs (browser, Frigate, ollama) live outside
  `/sys/fs/cgroup/schema-init/*`. Faster detection does not change what can be
  frozen (see mem-pressure reclaim design).

## Verification

1. `make test`: new `pressure_step` unit test, and `test_reclaim` unchanged.
2. schema-vmtest: boot as PID 1, boot log shows `psi trigger armed`. Run the
   scratch-cgroup hog inside the VM, then check `rail.log`: `freeze` ≤ 2 s
   after hog start, `thaw` ≤ 5 s after hog end, exactly one `reclaim` burst.
3. Fallback in the VM: boot with `psi=0`. Boot log shows avg10 mode, and
   the same hog still freezes (at the old ~6 s).
4. Deploy is a reboot, since this is a PID 1 binary change. On blakbox, repeat
   the 09-30 probe and compare against the table above.

## Open questions for Jonathan

1. Threshold `some 150000 1000000` (150 ms stalled per 1 s) is the kernel-doc
   example and fired on the first window here. An interactive desktop might
   want `some 100000 1000000`, and a laptop on HDD (DBox) might want looser.
   Start with the one we measured?
2. Faster thaw means peripherals (chronyd, avahi, docker, nfs-server) cycle
   freeze/thaw more readily under bursty load. Is ≥ 2 s silence enough, or
   should thaw wait longer (e.g. 5 s) than entry?
