# `.svc` reference

Every key a schema-init service file understands. The [README](how-it-works.md#service-files) has the basics and a few examples.

- [Keys](#keys)
- [Environment and pre-start commands](#environment-and-pre-start-commands)
- [Conditions](#conditions)
- [Socket activation](#socket-activation)
- [Drop-ins](#drop-ins)
- [Hardening](#hardening)
- [Service templates](#service-templates)
- [Group files](#group-files)
- [Timers](#timers)

## Keys

| Key | Default | Description |
|-----|---------|-------------|
| `name` | *(required)* | Service name — used in logs, dep resolution, and schema-ctl commands |
| `exec` | *(required)* | Absolute path to the binary to execute |
| `args` | — | One argument per line (repeat the key for multiple args), at most 62; more refuses the unit |
| `dep` | — | Dependency by name (repeat for multiple deps, up to 8; can name a service or a group). More than 8 refuses the load. |
| `oneshot` | `0` | Exit 0 → PERFECT and don't restart; exit non-zero → RECOVERY arc |
| `needs_root` | `0` | Abort spawn if uid ≠ 0 |
| `critical` | `0` | If `1`: service never reaches EXCISED — stays DORMANT at 1h retry indefinitely. Also: if this service is EXCISED, its dependents are hard-blocked. |
| `no_restart` | `0` | Any death → EXCISED immediately; no recovery arc |
| `max_restarts` | `5` | Maximum number of crash-driven restarts before entering EXCISED or backoff. The first spawn, timer firings and `schema-ctl restart` do not count; a run of 5 minutes or more before dying resets the count |
| `stable_secs` | `10` | Seconds process must stay alive before FULL_TRUST promotes to FUNDAMENTAL. Set lower for fast services; use `ready_path` instead when possible |
| `ready_path` | — | Filesystem path that, when it exists, triggers immediate FULL_TRUST→FUNDAMENTAL promotion. Falls back to `stable_secs` if the path never appears. A file already at the path when the service spawns (left by a previous run) does not count until it is replaced or rewritten, so a respawn is not promoted on its predecessor's marker or socket. Directories are exempt. In FUNDAMENTAL it also acts as a liveness probe: if the path *disappears*, the service is killed and backed off. The disappearance check only arms once the path has been seen at least once — a service promoted by `stable_secs` before its path exists won't be falsely killed. For services slower than `stable_secs` to come up (e.g. NetworkManager writing `resolv.conf`), set `stable_secs` generously so promotion doesn't outrun the path. |
| `oom_score_adj` | — | Written to `/proc/self/oom_score_adj` before exec (`-1000`..`1000`); e.g. `-900` keeps the kernel OOM killer off the bus, logind or udev. **Every child inherits it**, so never set it on a session launcher (`sddm`, autologin) or anything that spawns user apps — they would become unkillable. No value = the kernel default. schema-dbus resets its activated helpers to `0`. |
| `ready_bus_name` | — | Promote FULL_TRUST→FUNDAMENTAL as soon as this well-known **system-bus** name gets an owner inside the service's cgroup (systemd `Type=dbus` / `BusName=`). The schema-dbus broker reports acquisitions to PID 1 over `/run/schema-init/notify`; PID 1 accepts such reports only from root running `/usr/bin/schema-dbus`. Falls back to `ready_path`, then `stable_secs` (e.g. with another bus broker). |
| `notify` | `0` | `notify=1`: the service gets `NOTIFY_SOCKET` (`/run/schema-init/notify`) and promotes FULL_TRUST→FUNDAMENTAL the moment it sends `READY=1` (the `sd_notify` protocol, as systemd `Type=notify` daemons already do). A message counts only if the kernel-attested sender pid is in the service's cgroup; messages carrying fds are dropped. Falls back to `ready_path`, then `stable_secs`, so a daemon that never notifies behaves as before. `STATUS=` is recorded. A sender that has already exited can't be attributed (its pid may be reused), so a send-and-exit helper such as `systemd-notify --no-block` falls back too; plain `systemd-notify --ready` is attributed. |
| `pid_file` | — | For a daemon that forks into the background (systemd `Type=forking` with `PIDFile=`). When the launcher exits 0, PID 1 reads the daemon's PID from this file and supervises that process from then on: it counts as ready, its death enters the recovery arc, and stop and shutdown signal it. The PID must be alive and, when the service has a cgroup, inside it, so a stale or planted file is refused. A file written after the launcher exits is waited for up to 5 s; if none is valid by then the service's cgroup is killed and it retries. Needs the service's cgroup: without one there is nothing to check the PID against or to clean up with, so the service is excised (`pid_file-no-cgroup`). |
| `watchdog_sec` | `0` | Service watchdog (systemd `WatchdogSec=`). With `notify=1` the service gets `WATCHDOG_USEC`/`WATCHDOG_PID` and must send `WATCHDOG=1` (or be `schema-ctl pet`ted) at least every N seconds, counted from `READY=1`. A missed window sends SIGABRT (core dump via schema-coredump); still alive 90 s later, SIGKILL. The window is the one the running process was started with — a reload or re-exec that adds or changes `watchdog_sec` applies from the next spawn. A service being stopped is not watched. Either way it then restarts like any crash. The clock is held while the service is frozen under memory pressure. schema-import maps `WatchdogSec=` on `Type=notify` units. `0` = off. |
| `watchdog_timeout_ms` | `0` | Dead Man Token window in milliseconds. Service must call `schema-ctl pet <name>` (or send `WATCHDOG=1`) within this window or PID 1 stops kicking `/dev/watchdog` and the hardware resets. Use for `critical=1` real-time processes. `0` = disabled. |
| `cpu_limit` | `0` | Percent of one CPU core (1–100) enforced via cgroupv2 `cpu.max`. Written before child exec. `0` = unlimited. |
| `mem_limit` | `0` | Memory hard cap in MB via cgroupv2 `memory.max`. OOM inside the cgroup kills the service, not the system. Written before child exec. `0` = unlimited. |
| `priority` | `standard` | CPU contention class via cgroupv2 `cpu.weight`: `critical` (weight 1000), `standard` (100), `peripheral` (10). Proportional share — only takes effect when cores are saturated; idle services are never penalized. The analog of systemd's `CPUWeight=`. Children inherit the service's cgroup, so tagging a session leader (e.g. `display-manager`) elevates its whole subtree, compositor included. |
| `cpuset` | — | CPU affinity list pinning the service to specific cores via cgroupv2 `cpuset.cpus` (e.g. `2,3` or `4-7`). Constrains *where* the service may run, complementing `priority`/`cpu_limit` which govern *how much*. The analog of systemd's `AllowedCPUs=`. Requires the kernel's cgroupv2 `cpuset` controller (delegated automatically); if absent the setting no-ops. Empty = unconstrained (inherits the parent's CPUs). Useful for isolating a latency-sensitive control loop from cores that bursty background work hammers. |
| `cpuset_partition` | `member` | Exclusivity tier for the `cpuset=` cores via cgroupv2 `cpuset.cpus.partition`. `member` (default) = plain pinning, cores stay shared (no-op). `root` = the cores become an *exclusive* partition (no other service may run on them) while still being scheduler-load-balanced. `isolated` = exclusive **and** removed from the scheduler's load balancer — dynamic `isolcpus=`, no kernel cmdline needed; the target for a latency-critical control loop (the Ungulate Leg) or the audio path. Implemented as a cgroup v2 *remote partition*: schema-init reserves the cores in its own `cpuset.cpus.exclusive` and the service forms the partition, so the other services are unaffected. If the kernel rejects the partition (overlapping cores between two isolated services, or no cpuset controller) the service silently degrades to plain `cpuset=` pinning and a `HAZARD` line is logged — boot is never blocked. Requires a non-empty `cpuset=`; setting it alone is ignored with a warning. |
| `allowed_slot_min` | `-1` | Minimum hardware slot ID (inclusive) this service is permitted to run on. Checked against `SLOT_ID` env at spawn time. `-1` = unconstrained. |
| `allowed_slot_max` | `-1` | Maximum hardware slot ID (inclusive). If `SLOT_ID` falls outside `[allowed_slot_min, allowed_slot_max]`, spawn is refused with a `HAZARD` log and `SVC_NO_RESTART` is set — the service will not retry. Both min and max must be ≥ 0 to activate the gate. |
| `on_boot_sec` | `0` | Makes the service a **timer**: seconds after boot before the first fire (`0` = at boot). Implies `oneshot=1` — the service runs, exits, and re-arms. The analog of systemd's `OnBootSec=`. See [Timers](#timers) below. |
| `on_active_sec` | `0` | Timer period: seconds after each completion before the next fire. Measured from completion (like systemd's `OnUnitInactiveSec=`), so a slow run never overlaps itself. Implies `oneshot=1`. |
| `on_calendar` | — | Wall-clock timer: `HH:MM` daily, `Mon HH:MM` weekly, or `DD HH:MM` monthly. Implies `oneshot=1`. See [Timers](#timers). |
| `persistent` | `0` | With `on_calendar`: a fire missed while the machine was off runs at the next boot (systemd `Persistent=`). |
| `start_timeout_sec` | `90` for oneshots, `0` otherwise | Max seconds a service may sit in `FULL_TRUST` without promoting before it is killed and routed into the recovery arc — so a hung boot service can't stall its dependents. **Defaults on for oneshots** (the only services that can hang the chain; daemons promote via `stable_secs`). **Timers are exempt** (may run long). `0` disables. The analog of systemd's `TimeoutStartSec=`. |
| `stop_timeout_sec` | `3` | Seconds a service gets to exit after SIGTERM at shutdown before its cgroup is killed (1–300). Raise it for anything that saves state on the way out: the Plasma session gets `20`, because plasmashell can need 12 s to finish writing a freshly built panel layout. The analog of systemd's `TimeoutStopSec=`. |
| `stop_first` | `0` | At shutdown, this service gets SIGTERM before any other, and nothing else is stopped until it has exited (or its `stop_timeout_sec` ran out). For the desktop session: its apps talk to system daemons on the way out (KDE's printer client to CUPS, for one) and hang if those are stopping at the same time. systemd gets the same effect by stopping user sessions before system services. |
| `user` | — | Run as this user — a name or a numeric uid — with its primary group and supplementary groups, instead of root. `HOME`, `USER`, `LOGNAME`, `SHELL` and `XDG_RUNTIME_DIR` are set from the account (an `env=` line still wins). Resolved when the file is loaded; a user that does not exist yet is looked up again at start, and if it is still missing the start fails rather than running as root. |
| `limit_<name>` | — | Resource limit set before `user=` drops privileges, systemd's `Limit<NAME>=`: `limit_nofile`, `limit_memlock`, `limit_rtprio`, `limit_nice`, `limit_core`, `limit_stack`, `limit_nproc`, `limit_as`, `limit_cpu`, `limit_fsize`, `limit_data`, `limit_rss`, `limit_locks`, `limit_sigpending`, `limit_msgqueue`, `limit_rttime`. Value `N`, `N` with a `K`/`M`/`G`/`T` suffix (1024), or `infinity`; `SOFT:HARD` sets the two apart. `limit_nice` also takes a nice level (`-19`). `limit_nofile=infinity` means `fs.nr_open`. A bad value rejects the service; a limit that cannot be set fails the start. An empty value in a drop-in removes the limit. |
| `env` | — | `KEY=VALUE` set in the child before exec (repeat for several, up to 16 variables; a repeated key replaces the earlier value; more refuses the load). There is no shell, so no expansion. |
| `no_excise` | `0` | Like `critical=1` for the excision gate only: the service backs off in DORMANT forever instead of reaching EXCISED, without hard-blocking its dependents the way a critical dep does. |
| `fuse` | `0` | Quarantine fuse: if any dependency enters FRICTION or EXCISED, kill this service and excise it at once (for something that must not run on a broken foundation). |
| `fuse_cmd` | — | Shell command (`sh -c`) run when the fuse trips. |
| `failsafe` | — | Shell command (`sh -c`) run when the service dies unexpectedly or hits `start_timeout_sec`: drive an actuator to a safe position, raise an alarm. `$INSTANCE` is set for template instances. One at a time per service. |
| `failsafe_timeout_ms` | `500` | The failsafe command is SIGKILLed after this long. |
| `ready_poll_hz` | loop rate | How often a FUNDAMENTAL service's `ready_path` liveness check runs, when it should be slower than the main loop. |
| *(default)* | | Services restart automatically through the F9/F6 recovery arc unless `no_restart` or `oneshot` is set |

More keys, by topic, below: [Environment and pre-start commands](#environment-and-pre-start-commands), [Conditions](#conditions), [Socket activation](#socket-activation), [Drop-ins](#drop-ins), [Hardening](#hardening), [Timers](#timers).

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

## Environment and pre-start commands

| Key | Default | Description |
|-----|---------|-------------|
| `env_file` | — | Read `KEY=VALUE` lines from this file into the service's environment at each spawn (systemd `EnvironmentFile=`). Repeatable, up to 4 files and 64 variables; only the first 64 KiB of a file is read. A leading `-` makes the file optional; a missing required file fails the spawn. |
| `exec_pre` | — | A command run before `exec` at every spawn (systemd `ExecStartPre=`). Absolute path, arguments split like a shell would with quotes but no shell; up to 8. Prefix `+` to run it as root before hardening and the `user=` drop; without it the command runs as the service's user, after hardening. Prefix `-` to ignore its failure; otherwise a failing command fails the spawn and the service enters the recovery arc. |
| `expand_args` | `0` | `1`: expand `$VAR` and `${VAR}` in `args` and `exec_pre` from the service's environment (`env`, `env_file`) at spawn. A bare `$VAR` splits into words; if the result passes 63 words (`args`) or 32 (`exec_pre`) the spawn fails instead of truncating. Off by default, since there is no shell. |

## Conditions

`condition=KIND:ARG`, repeatable up to 8, checked before every spawn (systemd `Condition*=`). If one doesn't hold, the service is not started and counts as done (PERFECT, logged `condition-skip`), not failed, so its dependents aren't blocked; a timer just re-arms. `!` negates (`path_exists:!/etc/foo`). A `|` before the argument puts the condition in an OR group: at least one `|` condition must hold, and every plain one must.

Kinds: `path_exists`, `path_exists_glob`, `path_is_dir`, `path_is_symlink`, `path_is_mount`, `path_is_rw`, `dir_not_empty`, `file_not_empty`, `file_is_exec`, `ac_power` (`true`/`false`), `kernel_cmdline` (a word on `/proc/cmdline` equal to the argument; an argument without `=` also matches `arg=anything`).

## Socket activation

PID 1 can open a service's sockets itself and hand them over the way systemd does: at fd 3 onward, with `LISTEN_FDS`, `LISTEN_PID` and `LISTEN_FDNAMES` (`<name>.socket`) set. PID 1 keeps its copies across restarts, so connections queue instead of failing while the service restarts.

| Key | Default | Description |
|-----|---------|-------------|
| `listen` | — | A socket to open: `stream:`, `dgram:`, `seqpacket:` or `fifo:` followed by `/path`, `@abstract`, `PORT`, `ADDR:PORT` or `[ADDR]:PORT`. Up to 4. |
| `listen_lazy` | `0` | `1`: bind the sockets at boot but don't start the service; it sits in PERFECT (`socket-wait`) and is started by the first connection. A lazy service that exits cleanly after running at least 2 s goes back to waiting; a quicker exit goes through the recovery arc instead of a wake loop. |
| `socket_mode` | `0666` | Octal permissions for a filesystem socket or FIFO. |
| `socket_user` / `socket_group` | root | Owner of a filesystem socket or FIFO. |

When shutdown starts, PID 1 drops its copies: a running service keeps serving until it is stopped, and a client of one that isn't running is refused at once instead of hanging.

## Drop-ins

Override part of a `.svc` without editing it: put `*.conf` files in `<name>.svc.d/` next to it. They are applied after the main file, in byte order of their names; a template instance (`foo@bar.svc`) first reads `foo@.svc.d/`, then its own. A scalar key replaces the earlier value. List keys (`args`, `env`, `env_file`, `exec_pre`, `condition`, `listen`, `dep`, `landlock_ro`, `landlock_rw`) append; an empty `key=` clears that list first. `name=` is not allowed in a drop-in. `schema-ctl cat <name>` prints the file and its drop-ins in apply order.

## Hardening

Per-service confinement, applied in the child between fork and exec. Each is opt-in per `.svc`; a host-wide switch can default them on.

| Key | Default | Description |
|-----|---------|-------------|
| `no_new_privs` | `0` | `PR_SET_NO_NEW_PRIVS`: setuid binaries and file capabilities can no longer raise privilege in the service or anything it runs. |
| `keep_caps` | — | Comma-separated capability names (`CAP_NET_BIND_SERVICE,CAP_SYS_TIME`); every other capability is dropped from the bounding set. An unknown name refuses the load. |
| `private_tmp` | `0` | Fresh `/tmp` and `/var/tmp` for this service (private mount namespace). |
| `protect_system` | `0` | `1`: `/usr`, `/boot`, `/efi` read-only. `full`: `/etc` too. |
| `protect_home` | `0` | `/home`, `/root` and `/run/user` replaced by empty read-only mounts. |
| `landlock_ro` / `landlock_rw` | — | One absolute path per line, repeatable (up to 16). If any is set, the service can reach **only** the listed paths ([Landlock](https://docs.kernel.org/userspace-api/landlock.html)): `_ro` allows read and execute beneath the path, `_rw` allows everything. Root is confined too. The service's `exec`, its shared libraries and anything it reads (`/etc/ld.so.cache`, `/proc`, `/dev/null`…) must be listed. A path that doesn't exist is skipped. The load is refused if `exec` isn't covered, or if `keep_caps` drops `CAP_SYS_ADMIN` without `no_new_privs=1` (the kernel requires one of them). Applied after the uid/gid lookups and just before `setuid`. In a drop-in, an empty `landlock_ro=` or `landlock_rw=` clears that list. Needs a kernel with Landlock enabled; without it the service fails to start. |

**Host default.** `/etc/schema-init/hardening-default` containing `on`, or `schema.hardening_default=1` on the kernel command line (which wins, both ways), turns `no_new_privs`, `private_tmp`, `protect_system=1` and `protect_home` on for every service that doesn't set them. An explicit `key=0` opts a service out. A defaulted `private_tmp` or `protect_home` that would hide the service's own `exec` or `ready_path` is dropped with a log line; an explicit one refuses the load. The switch is off unless you turn it on. `schema-ctl status <svc>` shows each knob's value and whether it came from the file, the default, or was dropped.

A hardening step that fails aborts the spawn (`HARDENING FAILED` in the service log) rather than running the service unconfined.

## Service templates

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

## Group files

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

Maximum 16 groups, 8 members per group; a group with more is held EXCISED, so services that depend on it wait. Names and members are matched at load time.

## Timers

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

**Wall-clock timers** — set `on_calendar=HH:MM` to fire at a fixed local time every day, `on_calendar=Mon HH:MM` for weekly, or `on_calendar=15 HH:MM` for a day of the month, the way you'd write a cron line. This is the form you want for "3am backup", "midnight log rotation", "nightly cert renewal":

```ini
name=nightly-backup
exec=/usr/local/bin/backup.sh
needs_root=1
on_calendar=03:00     # fire at 03:00 local time, every day
```

`on_calendar` re-evaluates the wall clock on every fire, so it tracks `CLOCK_REALTIME` (not monotonic) — DST shifts and NTP clock steps self-correct each cycle rather than drifting. Time is local (`/etc/localtime`). Malformed values are logged and ignored, never scheduled.

**Catch-up after downtime** — by default a job missed while the machine was off simply runs at its next occurrence. Add `persistent=1` to run it **once at boot** instead, if its scheduled time passed while the system was down (systemd `Persistent=true`):

```ini
name=nightly-backup
exec=/usr/local/bin/backup.sh
needs_root=1
on_calendar=03:00
persistent=1          # if 03:00 was missed while off, run at next boot
```

Last-run is stamped to `/var/lib/schema-init/timers/<name>.stamp`; at boot, if the most recent scheduled occurrence is newer than that stamp, the timer fires immediately (logged `timer-catchup`) instead of waiting. A never-run timer is seeded rather than replayed, so enabling one doesn't trigger a surprise fire on first boot. `persistent=1` only applies to `on_calendar` timers; on an interval timer it's logged and ignored.

**Not yet implemented:** several times per day, ranges and lists in one key. See `docs/timers-design.md`.

---
