# schema-init

A minimal PID 1 init system for Linux that supervises services through a weight-state machine instead of unit files and dependency graphs.

No systemd. No OpenRC. No journal daemon. No socket activation engine. Just a statically linked binary that mounts your filesystems, spawns your services in dependency order, and watches them — then gets out of the way.

**PID 1 footprint: 892 KB RSS, 1 thread.**

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
             EXCISED  (76 — gate closes)
```

Three probe families:

| Probe | Asked when | Checks |
|-------|-----------|--------|
| **F8** | Before first spawn | Binary exists, deps stable, memory safe, permissions met |
| **F9** | After death | Retry budget, cooldown window, memory, escalation path |
| **F6** | After recovery fails | Last-chance: can we even attempt a restart? |

If a service is marked `critical` and reaches EXCISED, it logs a system friction warning. Everything else just stops retrying and steps aside.

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

| Key | Description |
|-----|-------------|
| `name` | Service name (used in logs and dep resolution) |
| `exec` | Absolute path to binary |
| `args` | Argument (repeat for multiple args) |
| `dep` | Dependency by name (repeat for multiple deps) |
| `oneshot` | Exit 0 → PERFECT, don't restart |
| `needs_root` | Require uid 0 before spawning |
| `critical` | EXCISED → system friction warning |
| `no_restart` | Any death → EXCISED immediately, no recovery arc |
| *(default)* | Services restart automatically through the F9/F6 recovery arc unless `no_restart` or `oneshot` is set |

Dependencies are resolved by name at load time. A service stays in `NEW_PROCESS` until all its deps reach `FUNDAMENTAL`, `SETTLED`, or `PERFECT`.

---

## State glossary

| State | Meaning |
|-------|---------|
| `NEW_PROCESS` | Queued. Waiting for all deps to reach FUNDAMENTAL. No spawn attempt yet. |
| `FULL_TRUST` | Spawned. Watching for 10 seconds — if it stays alive, it promotes. |
| `FUNDAMENTAL` | Stable. Load-bearing. Other services can depend on it. |
| `SETTLED` | Stable, non-critical. Satisfies deps but generates no friction warnings if lost. |
| `RECOVERY` | Died unexpectedly. F9 probe running. May re-queue or escalate. |
| `FRICTION` | Recovery failed. F6 last-chance probe running. |
| `EXCISED` | Permanently removed. No restart, no retry. Gate closes. |
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

- **Runtime service removal not supported** — `schema-ctl add <path>` loads new services at runtime, but there is no remove command yet. Removing a service requires a restart of the init.
- **No cgroup management** — schema-init does not create or manage cgroups. Services inherit the root cgroup.
- **No socket activation** — services must manage their own sockets. There is no systemd-style socket hand-off.
- **No dependency cycle detection at runtime** — cycles stall in `NEW_PROCESS` indefinitely. Cycle detection runs at load time and drops to a rescue shell, but runtime cycle introduction via schema-ctl is not guarded.

---

## Filesystem setup

schema-init does not parse `/etc/fstab`. On boot it mounts the pseudo-filesystems directly:

| Mount | Type | Notes |
|-------|------|-------|
| `/` | remount rw | Kernel mounts rootfs read-only for fsck; schema-init remounts it writable before anything else |
| `/proc` | proc | nosuid, nodev, noexec |
| `/sys` | sysfs | nosuid, nodev, noexec |
| `/dev` | devtmpfs | nosuid, strictatime |
| `/run` | tmpfs | nosuid, nodev, mode=0755 |
| `/sys/fs/cgroup` | cgroup2 | nosuid, nodev, noexec, relatime |

If your system needs additional mounts (data partitions, network filesystems), run them as `oneshot` services before your other services depend on them.

---

## Building

```sh
make
```

Produces a fully static binary — no glibc version dependency, runs on any Linux kernel. Tested on:

- Debian Bookworm, kernel 6.1, x86_64 — headless and Cinnamon desktop
- Fedora 44, kernel 7.0, x86_64 — full KDE Plasma desktop, btrfs subvolume boot

```sh
# install as PID 1
cp schema-init /sbin/schema-init
ln -s /sbin/schema-init /sbin/init

# or pass to kernel directly
linux /boot/vmlinuz root=LABEL=my-root init=/usr/sbin/schema-init
```

---

## Real numbers

Tested on Dell Inspiron 3542 (Intel Core i3, 4GB RAM) running full Cinnamon desktop:

| Metric | schema-init | systemd (same hardware, Fedora) |
|--------|-------------|----------------------------------|
| PID 1 RSS | **892 KB** | ~8–15 MB |
| PID 1 threads | **1** | 20–30+ |
| RAM used at desktop | **~1.1 GB** | ~1.6–2.0 GB |
| Swap used | **0 MB** | 200–500 MB |
| Time to desktop | **not yet measured** | slower |

The gap is structural. schema-init spawns your services and then sits in a 250ms tick loop. There is no journal daemon, no dbus-broker, no socket activation layer, no unit file parser running in the background.

---

## Runtime control

`schema-ctl` is a control client that communicates with the running init over a Unix domain socket at `/run/schema-init.sock`.

```sh
sudo schema-ctl status          # full state dump for all services
sudo schema-ctl list            # names and current states only
sudo schema-ctl start <name>    # start a stopped or EXCISED service
sudo schema-ctl stop <name>     # send SIGTERM to a running service
sudo schema-ctl restart <name>  # stop + re-queue through the state machine
sudo schema-ctl add <path>      # load a new .svc file at runtime, no reboot needed
```

The socket is `chmod 0600` — root only. Build alongside the init binary:

```sh
make schema-ctl
sudo cp schema-ctl /usr/local/bin/schema-ctl
```

---

## Logs

schema-init writes to stderr, which the kernel connects to the console (typically `/dev/console` at boot). To persist logs, redirect in your init script or point a service at a log file:

```sh
exec /sbin/schema-init 2>/var/log/schema-init.log
```

Per-service output goes to the console by default. To capture a specific service's stdout/stderr, wrap it:

```sh
exec=/usr/local/bin/my-logger   # wrapper that redirects before exec
```

There is no journal daemon. Logs are plain text, always.

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

See [`distros/fedora-kde/README.md`](distros/fedora-kde/README.md) for full installation instructions and key fixes.

---

## Roadmap

- [x] Runtime service loading — `schema-ctl add <path>` loads a new service at runtime
- [x] login1 D-Bus stub — `schema-logind` restores KDE shutdown/restart buttons on no-systemd systems
- [x] event-driven main loop — signalfd for SIGCHLD + poll() with 250ms timeout; wakes on child death and ctl commands instead of busy-polling
- [x] Boot hang fix — dep_idx alignment bug in group dep resolution; poll() replaces epoll (PID 1 epoll deadlock on kernel 6.1.0-49)
- [x] Boot timing — `schema-ctl timing` reports kernel→PID1 handoff and per-service FUNDAMENTAL/PERFECT timestamps (CLOCK_MONOTONIC)
- [ ] Boot time measurement — formal numbers with a `systemd-analyze` equivalent
- [ ] ARM port — Ungulate Leg hardware target
- [ ] schema-desktop — SDL2 live service viewer shipping as part of the repo

---

## License

AGPL-3.0 for open source use.  
Commercial license available for embedding in proprietary systems — open an issue or contact via GitHub.

---

*Built by Jonathan Ayers. The schema was written before the code.*
