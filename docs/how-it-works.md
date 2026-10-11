# How schema-init works

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

FULL_TRUST promotes to FUNDAMENTAL on the first readiness signal the service has: `READY=1` over `sd_notify` (`notify=1`), its bus name appearing (`ready_bus_name=`), a path appearing (`ready_path=`), a forking daemon's launcher exiting 0 with a valid `pid_file=`, and otherwise surviving `stable_secs` (default 10).

Services marked `critical=1` never reach EXCISED — they enter DORMANT and retry at 1-hour intervals indefinitely. Non-critical services excise after 5 dormant cycles (~75 minutes). A dep marked `critical=1` that is EXCISED still blocks its dependents. A non-critical EXCISED dep is skipped — dependents proceed without it.

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

The most-used keys:

| Key | Default | What it does |
|-----|---------|-------------|
| `name` | *(required)* | Service name, used in logs, `dep=` and `schema-ctl` |
| `exec` / `args` | *(required)* / — | Absolute path to the binary; one `args=` line per argument |
| `dep` | — | Start after this service or group (repeat for several) |
| `oneshot` | `0` | Runs to completion; exit 0 is success and it is not restarted |
| `needs_root` / `user` | `0` / — | Require root, or run as this user |
| `critical` | `0` | Never give up on it; its failure blocks dependents |
| `ready_path` / `notify` / `ready_bus_name` | — | Tell PID 1 the service is ready (file appears, `sd_notify`, or bus name owned) instead of waiting `stable_secs` |
| `env` / `env_file` | — | Environment for the child |
| `on_calendar` / `on_active_sec` | — | Make it a timer |

**Every key** (limits, cgroups, CPU pinning, watchdogs, conditions, socket activation, drop-ins, hardening, templates, groups, timers) is in **[docs/svc-reference.md](svc-reference.md)**.

---

## State glossary

| State | Meaning |
|-------|---------|
| `NEW_PROCESS` | Queued. Waiting for all deps to reach FUNDAMENTAL. No spawn attempt yet. |
| `FULL_TRUST` | Spawned. Watching — promotes to FUNDAMENTAL on `READY=1` (`notify=1`), when its `ready_bus_name` gets an owner, when `ready_path` exists, or when `stable_secs` elapses, whichever comes first. |
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

Both run the same sequence; SIGTERM ends in `reboot(RB_POWER_OFF)`, SIGINT in `reboot(RB_AUTOBOOT)`. `schema-ctl poweroff` / `reboot` start it too.

1. Hold 500 ms, so a desktop or display manager can show the shutdown state.
2. SIGTERM container cgroups, and drop PID 1's copies of socket-activation sockets.
3. Stop `stop_first=1` services (the desktop session) before anything else, and wait for them.
4. Stop the rest in reverse dependency order: a service gets SIGTERM once nothing still running depends on it, then has its `stop_timeout_sec` (default 3 s) to exit before its cgroup is killed. A service is down only when its cgroup is empty. After 12 s, everything left gets SIGTERM at once.
5. Kill every service cgroup and container cgroup, then `kill(-1, SIGKILL)` for anything outside them.
6. Release the control and notify sockets, `sync` with a 10 s deadline (so one wedged filesystem can't hang shutdown), flush the shutdown log, remount everything read-only, and call `reboot()`.

Each step is logged to `/var/log/schema-init/shutdown.log`.

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
