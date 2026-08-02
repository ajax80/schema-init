# Memory-Pressure Reclaim for the Survival Executive

**Status:** Approved — build TDD, vmtest as PID 1, then reboot to deploy.
**Component:** `init.c` survival executive + `.svc` classification.

## Problem

schema-init already has a PSI-driven survival executive (`init.c:459+`): it reads
`/proc/pressure/memory`, and on pressure `execute_survival_posture(1)` freezes
`PRIO_PERIPHERAL` services and CPU-throttles `PRIO_STANDARD`, with hysteresis
(`init.c:1976`). It is wired into PID 1's tick loop and running live.

But on **memory** pressure it relieves nothing today:
1. **Classification gap** — zero services are `priority=peripheral`, so the freeze
   branch acts on nothing.
2. **Freeze ≠ reclaim** — `cgroup.freeze` stops execution but the process's pages
   stay resident until kswapd sweeps them; no *direct* memory is freed.

## Design

### 1. Bounded, non-blocking reclaim (init.c)

- **Pure function** `long reclaim_target(long current, long cap)`:
  returns `0` if `current < RECLAIM_FLOOR` (4 MiB), else `min(current/2, cap)`.
  Unit-tested in isolation.
- **`memory.reclaim` write** is *synchronous* — it blocks the writer until the
  kernel finishes reclaiming. It MUST NOT run in PID 1's context. The reclaim pass
  runs in a **forked helper** (matching the existing `execute_fuse_cmd` /
  `start_failsafe` fork pattern). The parent returns immediately; the existing
  SIGCHLD reaper collects the child.
- The helper iterates services, and for each **non-critical** service with a
  cgroup, reads `<cgroup>/memory.current`, computes `reclaim_target(current, CAP)`
  (CAP = 128 MiB), and if `> 0` writes the decimal byte count to
  `<cgroup>/memory.reclaim`, logging `"reclaim"`. `ENOENT`/errors are ignored
  (older kernels, transient) — best-effort, never fatal.

### 2. Wiring (init.c)

- In `execute_survival_posture(1)`: **after** the existing freeze/throttle loop
  (so peripheral services are already frozen → their pages are cold → reclaim is
  maximally effective), gate on memory pressure specifically
  (`read_system_mem_pressure() > 10.0`) and fork the reclaim pass.
- `execute_survival_posture(0)` is unchanged: reclaim is one-shot; paged-out
  memory faults back naturally on thaw. No undo state.

### 3. Classification (.svc)

Mark these `priority=peripheral` (pause-tolerant background services):
`avahi, containerd, docker, docker-modules, logger, heartbeat, chronyd`.

Explicitly left `standard`/`critical` (must stay live under pressure):
`sshd, dbus, network, network-manager, udev, polkitd, getty-tty1,
schema-logind, schema-systemd1, hostname, display-manager, pipewire`.

## Guarantees / safety

- **PID 1 never blocks** on reclaim (forked helper).
- **Reversible** — freeze thaws; reclaim just pages out cold pages that fault back.
- **Critical services untouched** by both freeze and reclaim.
- **Best-effort** — any cgroup/file error is a no-op, never crashes PID 1.

## Scope caveat (honest)

schema-init can only reclaim from cgroups it manages
(`/sys/fs/cgroup/schema-init/*`). The largest RAM users on blakbox
(helium, frigate, ollama) are user-session apps *outside* those cgroups, so this
policy frees RAM from the **background service fleet**, not the browser. Real but
bounded. Bringing user apps under schema-init cgroups is out of scope.

## Deploy

- SIGHUP cannot deploy this: the integrity gate (`init.c:1348`) rejects changed
  existing `.svc` files ("takes effect at next boot"), and there is no PID 1
  re-exec path for the new binary.
- Therefore: recompile → **`schema-vmtest`** (boot as PID 1 in QEMU, induce
  pressure, assert freeze + reclaim + clean thaw) → **one reboot** of blakbox
  (operator-initiated; the agent stops before rebooting).

## Tasks

1. `reclaim_target` pure fn + C unit test (TDD). Add to `tests/`, wire into Makefile/runner.
2. `reclaim helper` (forked) + `memory.reclaim` writer in `init.c`.
3. Wire reclaim pass into `execute_survival_posture(1)` (mem-pressure-gated, post-freeze).
4. Classification: add `priority=peripheral` to the 7 `.svc` files.
5. Build; run existing unit/live tests + the new one green.
6. `schema-vmtest`: PID 1 boot + pressure scenario asserting freeze/reclaim/thaw.
7. Commit; STOP and report green for operator reboot.
