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
| `exec` | *(required)* | Absolute path to the binary to execute. It runs through `execv`, with no PATH search: a relative path or a bare name runs whatever is at that path under `working_dir=` (or `/`). So one is refused: the service is not loaded, and `schema-ctl verify` fails the next boot on it. A service already running with one (loaded by v0.4.34 or older) is kept through a re-exec or reload, with a warning, and refused at the next boot. `schema-import` resolves `ExecStart=`'s bare names to a path |
| `args` | — | One argument per line (repeat the key for multiple args), at most 62; more refuses the unit |
| `dep` | — | Dependency by name (repeat for multiple deps, up to 8; can name a service or a group). More than 8 refuses the load. |
| `after` | — | Ordering only, systemd's `After=`: hold this service until the named one has *finished its first start*, either way: up, failed once (in recovery or retrying), given up on, parked (`listen=` socket activation, a timer, a condition skip), or unable to start at all (waiting on a `wait_socket=`, or on a critical dep or group that is excised or dormant, directly or through its deps). A name with no `.svc` is ignored, and the named service's failure never holds or fails this one, unlike `dep=`. Reverses at shutdown: this one stops first. Up to 16; more refuses the load. An edge that would close a cycle (through `dep=` or other ordering) is dropped with a log line, as systemd drops it. |
| `before` | — | The same edge written on the other end: `before=X` in A orders X after A. Up to 16. |
| `oneshot` | `0` | Exit 0 → PERFECT and don't restart; exit non-zero → RECOVERY arc |
| `needs_root` | `0` | Abort spawn if uid ≠ 0 |
| `critical` | `0` | If `1`: service never reaches EXCISED — stays DORMANT at 1h retry indefinitely. Also: if this service is EXCISED, its dependents are hard-blocked. |
| `no_restart` | `0` | Any death → EXCISED immediately; no recovery arc. Same as `restart=no`. A later `restart=` other than `no` lifts it; `no_restart=0` does so only in a drop-in (in a base file `=0` is a no-op, as for every flag). |
| `restart` | `always` | Which deaths restart (systemd `Restart=`): `always`, `no`, `on-success`, `on-failure`, `on-abnormal`, `on-abort`, `on-watchdog`, with systemd's table. A clean exit is exit 0, a `success_exit_status=` code or signal, or SIGHUP/SIGINT/SIGTERM/SIGPIPE; `on-failure` restarts everything else (exit codes, other signals, watchdog, a lost readiness or `MAINPID=` wait), `on-abnormal` signals, watchdog and timeouts, `on-abort` other signals only, `on-watchdog` only a watchdog kill. A death that stays down goes EXCISED, as with `no_restart=1`: exit 0 or a success code reads as stopped (reason none), anything else keeps its reason. `schema-ctl start`/`restart` still start it. `start_timeout_sec` keeps excising a non-critical service whatever this says. `schema-import` maps `Restart=` (`no` and unset stay `no_restart=1`, `always` is the default). In a drop-in, an empty value goes back to `always`. |
| `success_exit_status` | — | Extra exit codes (0–255) and signal names (`SIGUSR1`, `TERM`…) that count as a clean exit (systemd `SuccessExitStatus=`), space separated; repeatable, lines add up. A `oneshot=1` service exiting with one is PERFECT. An unknown token refuses the load. In a drop-in, an empty value clears the list. |
| `restart_prevent_exit_status` | — | Exit codes and signals that never restart, whatever `restart=` says (systemd `RestartPreventExitStatus=`); not applied to watchdog or timeout deaths. Same syntax and drop-in rule as `success_exit_status`. |
| `kill_mode` | — | How a stop signals the service (systemd `KillMode=`). Unset: `schema-ctl stop` signals the main process and kills what is left once it is gone (`restart` does that only for a `pid_file=` or `main_pid_notify=1` service); shutdown signals every process at once. `control-group` (or `cgroup`): every process gets `kill_signal`, at stop and shutdown; the rest keep the stop's grace after the main process exits, a restart waits for them, and whatever is still there at the deadline gets `final_kill_signal`; what a dying main process leaves goes with it. `mixed`: the main process gets `kill_signal`, the rest `final_kill_signal` once it is gone. `process`: only the main process is signalled, on a stop, a restart, a `bind=` stop or a start-timeout kill; anything else it started keeps running (sshd: stopping it keeps the sessions). Child cgroups a service made are always removed when it exits, or the next run could not start. `schema-import` maps `KillMode=` (`none` becomes `process`); unset stays unset. In a drop-in, an empty value goes back to unset. |
| `kill_signal` / `final_kill_signal` | `TERM` / `KILL` | The stop signal and the one that follows when the service is still up after `stop_timeout_sec` (systemd `KillSignal=` / `FinalKillSignal=`): a name (`SIGINT`, `QUIT`) or a number; `STOP` refuses the load. A `schema-ctl stop`/`restart` that the service outlives escalates after `stop_timeout_sec`, or 90 s when that is unset (shutdown keeps its 3 s default). `schema-ctl`'s reply names the signal sent. `schema-import` maps both. |
| `max_restarts` | `5` | Maximum number of crash-driven restarts before entering EXCISED or backoff. The first spawn, timer firings and `schema-ctl restart` do not count; a run of 5 minutes or more before dying resets the count. `schema-import` writes systemd's `StartLimitBurst=` minus one here (the window is not kept) |
| `restart_sec` | `0` | Seconds to wait after a death before the recovery arc respawns the service (1–3600; `0` = at once). A service that fails straight away, for instance because the network isn't up yet, otherwise uses all its `max_restarts` within one pass and goes DORMANT for 300 s. `schema-ctl start`/`restart`/`reset` skip the wait; a held service logs `restart-hold`. Values above 3600 are clamped to 3600. The analog of systemd's `RestartSec=`, which `schema-import` translates (rounded up to whole seconds). |
| `stable_secs` | `10` | Seconds process must stay alive before FULL_TRUST promotes to FUNDAMENTAL. Set lower for fast services; use `ready_path` instead when possible |
| `ready_path` | — | Filesystem path that, when it exists, triggers immediate FULL_TRUST→FUNDAMENTAL promotion. Falls back to `stable_secs` if the path never appears. A file already at the path when the service spawns (left by a previous run) does not count until it is replaced or rewritten, so a respawn is not promoted on its predecessor's marker or socket. Directories are exempt. In FUNDAMENTAL it also acts as a liveness probe: if the path *disappears*, the service is killed and backed off. The disappearance check only arms once the path has been seen at least once — a service promoted by `stable_secs` before its path exists won't be falsely killed. For services slower than `stable_secs` to come up (e.g. NetworkManager writing `resolv.conf`), set `stable_secs` generously so promotion doesn't outrun the path. |
| `oom_score_adj` | — | Written to `/proc/self/oom_score_adj` before exec (`-1000`..`1000`); e.g. `-900` keeps the kernel OOM killer off the bus, logind or udev. **Every child inherits it**, so never set it on a session launcher (`sddm`, autologin) or anything that spawns user apps — they would become unkillable. No value = the kernel default. schema-dbus resets its activated helpers to `0`. |
| `stdout` | `log` | Where the service's standard output goes (systemd `StandardOutput=`): `log` (its log file), `null`, or a file. `file:PATH` writes from the start without truncating, `append:PATH` appends, and `truncate:PATH` empties the file first. PATH is absolute and under 128 bytes; it is opened as root (mode `0644` when created) before the service drops its user. A file that won't open fails the spawn, with the reason in the log. Applies to `exec_pre`, `exec`, `exec_stop`, `exec_stop_post`, `exec_reload` and `card` commands. Anything else refuses the load. `schema-import` maps `null`, `file:`, `append:` and `truncate:`; `journal`, `kmsg` and `syslog` are the log, and the `+console`, `tty`, `socket` and `fd` forms are warned. |
| `stderr` | follows `stdout` | The same for standard error (systemd `StandardError=`), plus `inherit`, the default: whatever `stdout` gets. `stderr=log` keeps it in the log under a redirected `stdout`. |
| `ignore_sigpipe` | `1` | `0` puts SIGPIPE back to its default action before exec, so a write to a closed pipe kills the process (systemd `IgnoreSIGPIPE=no`). By default it stays ignored, as PID 1 ignores it and exec keeps an ignored signal ignored; that is systemd's default too. Applies to every command of the service. |
| `keyring_mode` | `inherit` | The kernel session keyring (systemd `KeyringMode=`). `private` gives the service a new anonymous session keyring, `shared` the same with the user's keyring linked in; `inherit` keeps the one it was spawned with. Set up after the user switch, so the keyring belongs to the service's uid, for `exec` and the plain `exec_pre` lines (not `+` lines or the stop and reload commands). A kernel without keys, or a filter that denies `keyctl`, leaves it inherited, as systemd does. Note systemd's default for system services is `private`; `schema-import` writes only an explicit `KeyringMode=`. |
| `wait_socket` | — | Absolute path of an `AF_UNIX` stream socket. Once its deps are ready the service is held, unstarted, until a connect to that path succeeds; a socket file left behind by a dead listener does not count. Rechecked once a second; `schema-ctl status` shows `waiting=<path>` meanwhile. Used to start session services only once the session bus is up. |
| `ready_bus_name` | — | Promote FULL_TRUST→FUNDAMENTAL as soon as this well-known **system-bus** name gets an owner inside the service's cgroup (systemd `Type=dbus` / `BusName=`). The schema-dbus broker reports acquisitions to PID 1 over `/run/schema-init/notify`; PID 1 accepts such reports only from root running `/usr/bin/schema-dbus`. Falls back to `ready_path`, then `stable_secs` (e.g. with another bus broker); once PID 1 has accepted a name report this boot (the broker or `schema-namewatch` is live), the `stable_secs` fallback waits up to 90 s for the name instead, as systemd waits for `BusName=`, so a slow claimer's dependents don't start before it is reachable. |
| `notify` | `0` | `notify=1`: the service gets `NOTIFY_SOCKET` (`/run/schema-init/notify`) and promotes FULL_TRUST→FUNDAMENTAL the moment it sends `READY=1` (the `sd_notify` protocol, as systemd `Type=notify` daemons already do). A message counts only if the kernel-attested sender pid is in the service's cgroup; messages carrying fds are dropped. Falls back to `ready_path`, then `stable_secs`, so a daemon that never notifies behaves as before. `STATUS=` is recorded. A sender that has already exited can't be attributed (its pid may be reused), so a send-and-exit helper such as `systemd-notify --no-block` falls back too; plain `systemd-notify --ready` is attributed. |
| `main_pid_notify` | `0` | With `notify=1`: the service may name its main process with `MAINPID=<pid>` (systemd `Type=notify` + `NotifyAccess=all`, as `podman run -d --sdnotify=conmon` does). The pid must be alive and inside the service's cgroup or one below it, else the message is ignored and logged as `main-pid-rejected pid=<n> <why>`. Sent while the launcher still runs, it is kept until the launcher exits 0, then that pid is supervised: its death enters the recovery arc, stop and shutdown signal it. A launcher that exits 0 before any `MAINPID=` gets 10 s for one to arrive (`main-pid-timeout`, then the recovery arc); one that exits non-zero takes its cgroup with it. A later `MAINPID=` moves supervision to the new pid. The load is refused without `notify=1`. `schema-import --user` sets it for `Type=notify` units with `NotifyAccess=all`. |
| `verify_memory_max` / `verify_cpu_max` / `verify_pids_max` | — | Limits a service sets up for itself in a cgroup below its own (podman `--cgroups=split` puts the container in `<svc>/libpod-payload-<id>`). When the service sends `READY=1`, some populated cgroup below it must hold these values in `memory.max` (bytes; the kernel's rounding down to a page is allowed), `cpu.max` (`quota period`) and `pids.max`; if none does, or the service has no cgroup, the rail logs `limits-mismatch <file> want=… got=…`, the service's cgroup is killed and it takes the recovery arc — it never runs with the limit missing. Needs `notify=1`. `schema-import --user` writes them for Quadlet units from podman's `--memory`/`-m`, `--cpus` and `--pids-limit`. |
| `pid_file` | — | For a daemon that forks into the background (systemd `Type=forking` with `PIDFile=`). When the launcher exits 0, PID 1 reads the daemon's PID from this file and supervises that process from then on: it counts as ready, its death enters the recovery arc, and stop and shutdown signal it. The PID must be alive and, when the service has a cgroup, inside it, so a stale or planted file is refused. A file written after the launcher exits is waited for up to 5 s; if none is valid by then the service's cgroup is killed and it retries. Needs the service's cgroup: without one there is nothing to check the PID against or to clean up with, so the service is excised (`pid_file-no-cgroup`). |
| `watchdog_sec` | `0` | Service watchdog (systemd `WatchdogSec=`). With `notify=1` the service gets `WATCHDOG_USEC`/`WATCHDOG_PID` and must send `WATCHDOG=1` (or be `schema-ctl pet`ted) at least every N seconds, counted from `READY=1`. A missed window sends SIGABRT (core dump via schema-coredump); still alive 90 s later, SIGKILL. The window is the one the running process was started with — a reload or re-exec that adds or changes `watchdog_sec` applies from the next spawn. A service being stopped is not watched. Either way it then restarts like any crash. The clock is held while the service is frozen under memory pressure. schema-import maps `WatchdogSec=` on `Type=notify` units (capped at a day). `0` = off; above `86400` the service is refused. |
| `watchdog_timeout_ms` | `0` | Dead Man Token window in milliseconds. Service must call `schema-ctl pet <name>` (or send `WATCHDOG=1`) within this window or PID 1 stops kicking `/dev/watchdog` and the hardware resets. Use for `critical=1` real-time processes. `0` = disabled. A service loaded at boot with this set is one of the three ways to open the hardware watchdog; the others are `schema.watchdog=1` on the kernel command line or an `/etc/schema-init/watchdog` file. `schema.watchdog=0` keeps it closed regardless. With none of them PID 1 leaves `/dev/watchdog` alone. The choice is made once at boot (after the services load); a re-exec keeps whatever the running PID 1 held. |
| `cpu_limit` | `0` | Percent of one CPU core (1–10000) enforced via cgroupv2 `cpu.max`; above 100 the quota spans cores (`150` = 1.5 cores, systemd's `CPUQuota=150%`). Values above 100 were ignored before v0.4.26. Survival mode holds a `standard` service to half a core or this, whichever is tighter, and restores this after. Written before child exec. `0` = unlimited. |
| `cpu_weight` | — | cgroupv2 `cpu.weight` (1–10000) in place of the one `priority` would write — and in place of its `cpu.idle` for `peripheral`. Everything else still follows `priority`: `io.weight`, the nice value, and survival mode's freeze or half-core cap. The analog of systemd's `CPUWeight=`. Out of range refuses the file; empty = back to the tier. |
| `pids_max` | — | cgroupv2 `pids.max` (1–4194304): the most tasks the service's cgroup may hold; `fork()` past it fails. The analog of systemd's `TasksMax=`. Out of range refuses the file; `max`, `infinity` or empty = unlimited. |
| `mem_limit` | `0` | Memory hard cap via cgroupv2 `memory.max`: plain MB (`512`), `512M` or `2G`. Anything else (`512MB`, `1T`) refuses the service. OOM inside the cgroup kills the service, not the system. Written before child exec. `0` = unlimited. |
| `mem_low` | `0` | cgroupv2 `memory.low`: this much of the service's memory is reclaimed only after unprotected memory is gone (systemd `MemoryLow=`). Same units as `mem_limit`. The schema-init cgroup root's own `memory.low` is set to the sum of every loaded service's `mem_low`, because a child's protection only reaches as far as its parent's. `0` = none. |
| `swap_max` | — | cgroupv2 `memory.swap.max` (systemd `MemorySwapMax=`): MB (`0`, `512`, `512M`, `2G`), `max` or `infinity`. Unset, a `priority=critical` service gets `0` (never swapped: the audio stack and the compositor stay resident), the others keep the kernel's `max`. `swap_max=max` lets a critical service swap again. |
| `priority` | `standard` | CPU contention class via cgroupv2 `cpu.weight`: `critical` (weight 1000), `standard` (100), `peripheral` (10). Proportional share — only takes effect when cores are saturated; idle services are never penalized. The analog of systemd's `CPUWeight=`. Children inherit the service's cgroup, so tagging a session leader (e.g. `display-manager`) elevates its whole subtree, compositor included. |
| `cpuset` | — | CPU affinity list pinning the service to specific cores via cgroupv2 `cpuset.cpus` (e.g. `2,3` or `4-7`). Constrains *where* the service may run, complementing `priority`/`cpu_limit` which govern *how much*. The analog of systemd's `AllowedCPUs=`. Requires the kernel's cgroupv2 `cpuset` controller (delegated automatically); if absent the setting no-ops. Empty = unconstrained (inherits the parent's CPUs). Useful for isolating a latency-sensitive control loop from cores that bursty background work hammers. |
| `nice` | — | `-20`–`19` (systemd `Nice=`): the service's nice value, in place of the one `priority=` would set (its cgroup weight still follows `priority`). Lowering it needs root or `RLIMIT_NICE` (a unit's own `limit_nice=` counts); not granted, the service runs at the closest nice allowed, with a log line, as under systemd. Also applied to `exec_stop`, `exec_stop_post` and `exec_reload`. |
| `io_class` | — | `realtime`, `best-effort`, `idle` or `none` (systemd `IOSchedulingClass=`): the I/O scheduling class. `realtime` needs root. Not granted, the start fails. Also applied to the stop/reload commands. |
| `io_priority` | — | `0` (highest) – `7` within `realtime`/`best-effort` (systemd `IOSchedulingPriority=`); default `4`. Alone, it means `best-effort`. |
| `sched_policy` | — | `other`, `batch`, `idle`, `fifo` or `rr` (systemd `CPUSchedulingPolicy=`). `fifo`/`rr` are real-time: a control loop that must not wait behind ordinary work. They need root or `RLIMIT_RTPRIO` (a unit's own `limit_rtprio=` counts); not granted, the start fails rather than run unscheduled. A runaway `fifo` service can starve its CPU: pair it with `cpuset=`. |
| `sched_priority` | `1` | `1`–`99` for `fifo`/`rr` (systemd `CPUSchedulingPriority=`); with any other policy, or none, the file is refused. In a drop-in, an empty value clears any of the five scheduling keys. |
| `cpuset_partition` | `member` | Exclusivity tier for the `cpuset=` cores via cgroupv2 `cpuset.cpus.partition`. `member` (default) = plain pinning, cores stay shared (no-op). `root` = the cores become an *exclusive* partition (no other service may run on them) while still being scheduler-load-balanced. `isolated` = exclusive **and** removed from the scheduler's load balancer — dynamic `isolcpus=`, no kernel cmdline needed; the target for a latency-critical control loop (the Ungulate Leg) or the audio path. Implemented as a cgroup v2 *remote partition*: schema-init reserves the cores in its own `cpuset.cpus.exclusive` and the service forms the partition, so the other services are unaffected. If the kernel rejects the partition (overlapping cores between two isolated services, or no cpuset controller) the service silently degrades to plain `cpuset=` pinning and a `HAZARD` line is logged — boot is never blocked. Requires a non-empty `cpuset=`; setting it alone is ignored with a warning. |
| `allowed_slot_min` | `-1` | Minimum hardware slot ID (inclusive) this service is permitted to run on. Checked against `SLOT_ID` env at spawn time. `-1` = unconstrained. |
| `allowed_slot_max` | `-1` | Maximum hardware slot ID (inclusive). If `SLOT_ID` falls outside `[allowed_slot_min, allowed_slot_max]`, spawn is refused with a `HAZARD` log and `SVC_NO_RESTART` is set — the service will not retry. Both min and max must be ≥ 0 to activate the gate. |
| `on_boot_sec` | `0` | Makes the service a **timer**: seconds after boot before the first fire (`0` = at boot). Implies `oneshot=1` — the service runs, exits, and re-arms. The analog of systemd's `OnBootSec=`. See [Timers](#timers) below. |
| `on_active_sec` | `0` | Timer period: seconds after each completion before the next fire. Measured from completion (like systemd's `OnUnitInactiveSec=`), so a slow run never overlaps itself. Implies `oneshot=1`. |
| `on_calendar` | — | Wall-clock timer: `[DOW] [DOM] H:M`. DOW: weekday names (`Mon`, `monday`) with lists and ranges (`Sat,Sun`, `Mon..Fri`; ranges run Monday to Sunday as in systemd, so `Sat..Sun` is the weekend); DOM: `15`, `1,15`, `1..7`; H and M: `*`, a number, lists, ranges `a..b`, steps `a/s` or `*/s` (`*:0/15`; `8..17/2` stops at 17). With both DOW and DOM, both must hold (`Sun 1..7 01:00` = first Sunday). Up to 4 lines; the earliest next one fires. A drop-in's first `on_calendar=` replaces the base file's schedule. Implies `oneshot=1`. See [Timers](#timers). |
| `randomized_delay_sec` | `0` | Timers: each fire waits a random 0..N whole seconds more, drawn fresh at every arm (systemd `RandomizedDelaySec=`), so timers sharing a time don't start in the same second. 0..2678400. |
| `watch_path` | — | `KIND:PATH`, up to 8 lines (systemd `.path` units): the service starts when the filesystem says so, instead of at boot. `exists`, `exists_glob` (glob in the last component only) and `dir_not_empty` fire whenever the condition holds and the service isn't running, including again after a run; `changed` (closed after a write, created, deleted, renamed onto, attributes) and `modified` (any write too) fire on the event, and an event during a run queues one more run. PATH is absolute; a symlink leaf is watched as the link. Cannot be combined with `listen=`, a timer key or `pid_file=`. An empty `watch_path=` in a drop-in clears the list. See [Path watches](#path-watches). |
| `make_directory` | `0` | With `watch_path`: create each watched PATH as a directory at arm time, except for `exists` and `exists_glob` (as systemd's `MakeDirectory=`); parents 0755, owned by the process that creates it (root under PID 1). |
| `directory_mode` | `0755` | Mode for `make_directory`. |
| `trigger_limit_burst` | `200` | With `watch_path`: more fires than this within `trigger_limit_interval_sec` parks the service (`excise_reason=trigger-limit-hit`) until `schema-ctl start` or `restart`. |
| `trigger_limit_interval_sec` | `2` | The window for `trigger_limit_burst`, 1–86400. |
| `syscall_filter` | — | Seccomp filter (systemd `SystemCallFilter=`): syscall names and `@groups` (systemd's: `@system-service`, `@privileged`, `@mount`, …), each optionally `:ERRNO`. A plain first line is an allowlist, one starting with `~` a denylist; later lines add, an opposite-sign line takes names back out; empty clears. Unknown names are skipped with a log line (as systemd does); an unknown group refuses the load. See [Syscall filtering](#syscall-filtering). |
| `syscall_deny` | — | Schema's own denylist, merged into the `syscall_filter=` program: `default` is the preset (`@clock @cpu-emulation @debug @module @mount @obsolete @raw-io @reboot @swap kexec_load kexec_file_load bpf`), a list of names/`@groups` is exactly those, `0` is nothing. Unset is nothing: it is opt-in only and **not** turned on by the [host default](#hardening), so a new knob never tightens a `.svc` written before it existed. With no `syscall_filter=` it is a denylist; with a denylist the sets add; with an allowlist those names are taken out of it. `schema-import` and the shipped `.svc` write `syscall_deny=0` explicitly; it means the same as leaving it unset. |
| `syscall_error_number` | `EPERM` | What a blocked syscall returns: an errno name or number, or `kill`. **Default EPERM, not systemd's kill.** |
| `syscall_architectures` | — | `native` (systemd `SystemCallArchitectures=`). Any filter already refuses other ABIs. |
| `restrict_suid_sgid` | `0` | `1`: creating setuid/setgid files fails with EPERM (chmod family, open/creat with `O_CREAT` or `O_TMPFILE`, mknod). |
| `lock_personality` | `0` | `1`: `personality()` only with `PER_LINUX` (or the query). |
| `restrict_realtime` | `0` | `1`: switching to `SCHED_FIFO`/`RR`/`DEADLINE` fails with EPERM. The service's own `sched_policy=` still applies (set before the filter). |
| `restrict_namespaces` | `0` | `1`: `clone`/`unshare` with `CLONE_NEW*` and `setns` fail with EPERM (yes/no only). |
| `persistent` | `0` | With `on_calendar`: a fire missed while the machine was off runs at the next boot (systemd `Persistent=`). |
| `start_timeout_sec` | `90` for oneshots, `0` otherwise | Max seconds a service may sit in `FULL_TRUST` without promoting before it is killed and routed into the recovery arc — so a hung boot service can't stall its dependents. **Defaults on for oneshots** (the only services that can hang the chain; daemons promote via `stable_secs`). **Timers are exempt** (may run long). `0` disables. A non-critical service is excised on the timeout, unless `restart=` is set and its policy covers a timeout (`always`, `on-failure`, `on-abnormal`): then it restarts, as systemd's `Restart=` does. The analog of systemd's `TimeoutStartSec=`, which `schema-import` translates for notify, oneshot and forking units (and long ones on dbus). |
| `stop_timeout_sec` | `3` | Seconds a service gets to exit after its `kill_signal` at shutdown before its cgroup is killed (1–300); also how long a `schema-ctl stop`/`restart` waits before `final_kill_signal` (90 s when unset). Raise it for anything that saves state on the way out: the Plasma session gets `20`, because plasmashell can need 12 s to finish writing a freshly built panel layout. The analog of systemd's `TimeoutStopSec=`. |
| `stop_first` | `0` | At shutdown, this service gets SIGTERM before any other, and nothing else is stopped until it has exited (or its `stop_timeout_sec` ran out). For the desktop session: its apps talk to system daemons on the way out (KDE's printer client to CUPS, for one) and hang if those are stopping at the same time. systemd gets the same effect by stopping user sessions before system services. |
| `user` | — | Run as this user — a name or a numeric uid — with its primary group and supplementary groups, instead of root. `HOME`, `USER`, `LOGNAME`, `SHELL` and `XDG_RUNTIME_DIR` are set from the account (an `env=` line still wins). Resolved when the file is loaded; a user that does not exist yet is looked up again at start, and if it is still missing the start fails rather than running as root. |
| `group` | — | Run with this group — a name or a numeric gid — as the process gid (systemd `Group=`). Beside `user=` it overrides the user's primary group, whichever line comes first; alone it means root with that gid. Applied to `exec`, non-`+` `exec_pre`, `exec_stop` and `exec_stop_post`. Resolved at start like `user=`; still missing then fails the start rather than falling back to root:root. `schema-import` maps `Group=`, dropping one that only names the primary group it would get anyway. |
| `supplementary_groups` | — | Extra groups by name or gid, space-separated, up to 16 (systemd `SupplementaryGroups=`): added on top of the user's own `/etc/group` memberships, for a root service too. Later lines add; an empty one clears. An unknown group fails the start. |
| `dynamic_user` | `0` | `1`: run as a uid allocated for this service (systemd `DynamicUser=`) instead of a static user — see [Dynamic users](#dynamic-users). A `user=` that exists is used instead, with everything else still applying. Ignored by schema-user. |
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
| `working_dir` | — | Directory the service's commands run in (systemd `WorkingDirectory=`): `exec`, `exec_pre`, `exec_stop` and `exec_stop_post`. An absolute path, or `~` for the home of `user=` (root's if unset). The chdir happens as the service's user after hardening, so a `private_tmp=1` `/tmp` is the private one; `+` `exec_pre` lines run there too, as root, before hardening (in `/` if root can't enter it). A missing directory fails the spawn unless the value starts with `-`, in which case the commands run in `/`. `exec_stop` / `exec_stop_post` run outside the service's mount namespace, so a path inside a private `/tmp` isn't there for them; when the directory is missing they run in `/` and are never skipped. A defaulted `private_tmp` / `protect_home` that would hide the directory is dropped, like one that hides `exec`. In a drop-in, an empty value clears it. Unset, commands inherit the supervisor's directory (`/` under PID 1). `schema-import` maps `WorkingDirectory=`, resolving specifiers. |
| `exec_stop` / `exec_stop_post` | — | Commands run when the service stops (systemd `ExecStop=` / `ExecStopPost=`). Same form as `exec_pre`: absolute path, split like `args`, no shell, up to 8 each, a leading `-` ignores a failure; a failure without it skips the rest of that list. `exec_stop` runs on every stop — `schema-ctl stop`/`restart` and shutdown — before the main process gets SIGTERM; `exec_stop_post` runs once the service is gone, after a crash too, and a respawn waits for it. Both run as the service's user with its `env=`, in one child of their own; `stop_timeout_sec` (default 3) bounds them, then that child is killed and the stop goes on. On a reload that evicts the service both lists run at once beside its SIGTERM. `schema-import --user` maps `ExecStop=`/`ExecStopPost=`; system imports do not. In a drop-in, an empty value clears the list. |
| `exec_reload` | — | Commands `schema-ctl reload <svc>` runs on a running service (systemd `ExecReload=`). Same form as `exec_stop`: absolute path, no shell, up to 8, `-` ignores a failure, a failure without it skips the rest. `$MAINPID` and `${MAINPID}` anywhere in a word are replaced by the supervised pid, which is also in the environment as `MAINPID`. One child of their own, as the service's user with its `env=`; PID 1 replies once it is spawned and never waits for it. `stop_timeout_sec` (default 3) bounds it, then it is killed (`reload-timeout`). A reload never restarts the service or touches its state. Wins over `reload_signal`. In a drop-in, an empty value clears the list. |
| `reload_signal` | — | Signal `schema-ctl reload <svc>` sends the supervised pid when there is no `exec_reload`: `HUP`, `SIGUSR1`, `urg`, or a number 1–64. `KILL`, `STOP` and unknown names refuse the load. `reload=1` (the older systemd1-shim opt-in) means `reload_signal=HUP` unless a `reload_signal=` is also given. `schema-import` maps an `ExecReload=` that is only `kill -SIG $MAINPID` lines to this, other `ExecReload=` lines to `exec_reload`, and `Type=notify-reload` to its `ReloadSignal=` (default HUP). `systemctl reload` / `reload-or-restart` and the D-Bus `ReloadUnit` use these keys; a service with neither is restarted instead by `reload-or-restart`, and by the shim's `reload` too, as before. A reload that fails for another reason (already reloading, not running) is an error, never a restart. In a drop-in, an empty value clears it, `reload=1`'s HUP included. |
| `runtime_dir` / `state_dir` / `cache_dir` / `logs_dir` | — | Directories the service gets, created before every spawn (systemd `RuntimeDirectory=`, `StateDirectory=`, `CacheDirectory=`, `LogsDirectory=`). Space-separated relative paths, repeatable, up to 8 per key; nested ones are fine (`cockpit/tls`). No `.`, `..`, empty component or `:`; a bad entry refuses the load. Under PID 1 they go in `/run`, `/var/lib`, `/var/cache`, `/var/log`; under schema-user in `$XDG_RUNTIME_DIR`, `$XDG_STATE_HOME`, `$XDG_CACHE_HOME` and `$XDG_STATE_HOME/log` (`~/.local/state`, `~/.cache` when unset). Created before the `+` `exec_pre` lines: parents missing below the base are made root-owned `0755`, the directory itself is owned by `user=`/`group=` and given its mode on every spawn. Below the base PID 1 follows a symlink only when the directory holding it is owned by root (by the user, under schema-user) and writable by no one else: an admin's `/var/lib/mysql -> /data/mysql` works, a link planted in a parent the service user owns does not. An existing tree's contents are not re-owned. A runtime directory that can't be made fails the spawn; a state, cache or logs one is logged (`starting anyway`) and the service starts, as an existing one in a form PID 1 won't touch (read-only `/var`) must not stop it. The full paths, sorted and `:`-joined, are in `RUNTIME_DIRECTORY`, `STATE_DIRECTORY`, `CACHE_DIRECTORY`, `LOGS_DIRECTORY` for `exec`, `exec_pre`, `exec_stop`, `exec_stop_post` and `exec_reload`; an `env=` of the same name wins. The runtime directories are removed once the service is stopped for good (`schema-ctl stop`, a `restart=` policy that says no, given up, a `bind=` stop) and emptied by `schema-ctl restart`; a respawn after a crash, the dormant backoff and a finished oneshot keep them. The delete never follows a symlink or enters a filesystem mounted below the directory. Renaming or removing a `runtime_dir=` while the service runs, or removing the service from the config, leaves the old directory behind. The others stay. `schema-import` maps the four directives and their `*Mode=`; `ConfigurationDirectory=` stays an `exec_pre` line that creates it only if missing. In a drop-in, an empty value clears that list. |
| `runtime_dir_mode` / `state_dir_mode` / `cache_dir_mode` / `logs_dir_mode` | `0755` | Octal mode for that key's directories (not the parents); `0000` is honoured. In a drop-in, an empty value goes back to `0755`. |
| `dir_owner` | — | `USER[:GROUP]`: owns the `runtime_dir`/`state_dir`/`cache_dir`/`logs_dir` directories in place of the run user, which stays as `user=` says (root without one). Without `:GROUP`, the user's primary group. Looked up at each start, and a user or group that doesn't exist fails the spawn. Ignored under schema-user and with `dynamic_user=1`, whose private directories stay the allocated uid's. `schema-import` writes it for `ExecStart=!` / `+` with `User=` or `Group=` (`root:GROUP` with no `User=`), where systemd runs the process as root but still gives the directories to `User=`/`Group=` (Debian's chrony). In a drop-in, an empty value clears it. |
| `card` | — | A limp-mode card, played when the service fails and the recovery arc retries it; one card per retry, in file order, up to 4. **`card=exec:CMD`** runs CMD (absolute path, split like `exec_pre`, no shell) as root in its own process, with output in the service's log; the service is not started again until the card exits, and a card still running after 30 s is killed. A failing card is logged (`card-failed`) and the service is retried anyway. **`card=fallback:SVC`** names a standby service: SVC does not start at boot (the board shows it as STANDBY); playing the card starts it and parks this service with the usual dormant backoff. When this service wakes, SVC is stopped and this one starts only after SVC has exited (they may share a device); if this one goes dormant or is excised again, SVC comes back. `schema-ctl start SVC` starts a standby by hand. The deck is played once and starts over with the restart budget: after a 300 s run, or on `schema-ctl start`, `restart` or `reset`. `schema-ctl status` shows `cards=PLAYED/TOTAL`. In a drop-in, an empty `card=` clears the deck. |
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
| `bounding_caps` | — | Same list syntax as `keep_caps`: the bounding set keeps only these, which is systemd's `CapabilityBoundingSet=`. `~` in front drops only the named capabilities, so ones newer than schema-init's table stay. Empty is no capabilities at all. Unlike `keep_caps`, the permitted and effective sets are left alone, so `user=` and `group=` still work. A root service gets exactly the bounding set at `exec`. `keep_caps` is cut to it. Also applied to `exec_stop`, `exec_stop_post` and `exec_reload`. On a `user=` or `dynamic_user` service, an `ambient_caps` cap it drops refuses the load. An unknown name refuses the load. |
| `ambient_caps` | — | Same list syntax as `keep_caps`. With `user=` set, these capabilities are raised as ambient caps after the uid drop, so the program keeps them across `exec` (e.g. `CAP_NET_BIND_SERVICE` for a non-root DNS server on port 53). They are added to the bounding set when `keep_caps` is set. Ignored for root services. Also applied to `exec_pre` commands that run after the drop and to `exec_stop`. An unknown name refuses the load. |
| `private_tmp` | `0` | Fresh `/tmp` and `/var/tmp` for this service (private mount namespace). They are the empty directories `/tmp/schema-private/<name>/tmp` and `…/var-tmp` on the host, so they share the host `/tmp`'s filesystem and its size limit (not a tmpfs of their own, which would default to half the RAM). Emptied each time the service starts; between runs the files stay in `/tmp/schema-private` (root's, mode 0700). `exec_pre` and the stop commands run outside the namespace and see the host `/tmp`. Before the host `/tmp` is writable (read-only root early in boot) each gets a tmpfs capped at 10 % of RAM instead. |
| `protect_system` | `0` | `1`: `/usr`, `/boot`, `/efi` read-only. `full`: `/etc` too. |
| `protect_home` | `0` | `/home`, `/root` and `/run/user` replaced by empty read-only mounts. |
| `protect_kernel_tunables` | `0` | `/proc/sys`, `/sys`, `/proc/sysrq-trigger`, `/proc/latency_stats`, `/proc/acpi`, `/proc/apm`, `/proc/asound`, `/proc/bus`, `/proc/fs`, `/proc/irq`, `/proc/scsi`, `/proc/mtrr`, `/proc/timer_list` and `/proc/timer_stats` read-only; `/proc/kallsyms` and `/proc/kcore` read as empty, `/sys/kernel/debug` and `/sys/kernel/tracing` as empty unreadable dirs (systemd `ProtectKernelTunables=`). An `rw_path` under them makes that path writable again. |
| `protect_kernel_modules` | `0` | `CAP_SYS_MODULE` leaves the bounding set, even when `keep_caps` names it, and `/usr/lib/modules`, `/lib/modules` are empty read-only mounts (systemd `ProtectKernelModules=`). An `ambient_caps` naming `CAP_SYS_MODULE` refuses the load. To block the module syscalls too, add `syscall_deny=default` (it denies `@module`). |
| `protect_control_groups` | `0` | `/sys/fs/cgroup` read-only (systemd `ProtectControlGroups=`; `strict` imports as `1`, without a cgroup namespace). |
| `private_devices` | `0` | A tmpfs `/dev` (nosuid, noexec) holding only `null`, `zero`, `full`, `random`, `urandom`, `tty`, `shm`, `mqueue` and `log` from the host, a private `/dev/pts` with `/dev/ptmx`, and the `fd`/`stdin`/`stdout`/`stderr` links; `CAP_MKNOD` and `CAP_SYS_RAWIO` leave the bounding set (systemd `PrivateDevices=`). A `ready_path` under `/dev` refuses the load. No `DeviceAllow=` yet. `/dev/log` is the socket as it was at spawn: after `schema-journal-sink` restarts, the service's `syslog()` goes nowhere until it restarts too (its stdout/stderr still reach its log). |
| `protect_hostname` | `0` | The service gets its own hostname (UTS namespace), and `/proc/sys/kernel/hostname` and `domainname` are read-only: nothing it does renames the host (systemd `ProtectHostname=`; `private` imports as `1`, `yes:NAME` as `1` without setting the name). With `CAP_SYS_ADMIN`, `sethostname()` changes only the service's own copy; `syscall_filter=~sethostname setdomainname` blocks the call itself. |
| `private_network` | `0` | The service gets its own network namespace with only loopback, brought up (systemd `PrivateNetwork=`). `exec_stop`, `exec_stop_post` and `exec_reload` run in the same namespace (a fresh empty one once the main process is gone); `+` `exec_pre` lines run outside it. A user named by `user=` is looked up before the switch. Sockets PID 1 passes in (`listen=`) keep working, as do unix sockets reached by path (D-Bus, `/dev/log`); abstract unix sockets and every host interface are out of reach. `/sys/class/net` still lists the host's interfaces. |
| `memory_deny_write_execute` | `0` | No memory mapping may be writable and executable at once, or become executable later (systemd `MemoryDenyWriteExecute=`, through `PR_SET_MDWE`). Applies to the main process and the plain `exec_pre` lines (they run after hardening); only `+` lines and the stop/reload/card commands escape it. Needs Linux 6.3+: on an older kernel the start fails (`HARDENING FAILED … memory_deny_write_execute`) rather than run unprotected. JITs (Java, Node, LuaJIT, some Python extensions) break under it. |
| `umask` | — | Octal, `0`–`0777` (systemd `UMask=`): the file-creation mask for `exec_pre`, `exec`, `exec_stop`, `exec_stop_post`, `exec_reload` and `card` commands. An empty `umask=` in a drop-in clears it. Unset, they inherit PID 1's `0022`. |
| `rw_path` | — | One absolute path per line, repeatable (up to 8): stays writable inside a `protect_system` read-only mount (systemd's `ReadWritePaths=`), e.g. `protect_system=full` + `rw_path=/etc/pihole`. Only that path, not its parent. A path that doesn't exist is skipped. Does nothing without `protect_system`, `private_tmp`, `protect_home`, `protect_kernel_tunables` or `protect_control_groups`, and can't reach under `protect_home`'s empty mounts. In a drop-in, an empty `rw_path=` clears the list. |
| `landlock_ro` / `landlock_rw` | — | One absolute path per line, repeatable (up to 16). If any is set, the service can reach **only** the listed paths ([Landlock](https://docs.kernel.org/userspace-api/landlock.html)): `_ro` allows read and execute beneath the path, `_rw` allows everything. Root is confined too. The service's `exec`, its shared libraries and anything it reads (`/etc/ld.so.cache`, `/proc`, `/dev/null`…) must be listed. A path that doesn't exist is skipped. The load is refused if `exec` isn't covered, or if `keep_caps` drops `CAP_SYS_ADMIN` without `no_new_privs=1` (the kernel requires one of them). Applied after the uid/gid lookups and just before `setuid`. In a drop-in, an empty `landlock_ro=` or `landlock_rw=` clears that list. Needs a kernel with Landlock enabled; without it the service fails to start. |
| `landlock_bind_tcp` / `landlock_connect_tcp` | unrestricted | TCP ports the service may bind / connect to, enforced by the kernel ([Landlock](https://docs.kernel.org/userspace-api/landlock.html) network rules, kernel 6.7+): a comma- or space-separated list, repeatable, up to 16 ports each, or `none` (alone on its line, replacing any ports set before it, a drop-in's included) for no explicit TCP bind / connect at all. Setting one leaves the other unrestricted, and neither needs `landlock_ro`/`landlock_rw`. A refused `bind()` or `connect()` fails with `EACCES`, whatever the address. Port `0` in `landlock_bind_tcp` allows binding to an ephemeral port. TCP only: UDP, unix sockets and raw sockets are not affected. The kernel checks `bind()` and `connect()`, not `listen()`: a socket that calls `listen()` without binding first still gets an ephemeral port, even under `landlock_bind_tcp=none`. `schema-ctl status SVC` lists the rules as `bind_tcp` / `connect_tcp`. Same `no_new_privs` rule as above. On an older kernel the service fails to start rather than run unconfined. In a drop-in, an empty value lifts the restriction. Example, a web server that can only listen on 80/443 and only reach a local database: `landlock_bind_tcp=80,443` and `landlock_connect_tcp=5432`. |

**Host default.** `/etc/schema-init/hardening-default` containing `on`, or `schema.hardening_default=1` on the kernel command line (which wins, both ways), turns `no_new_privs`, `private_tmp`, `protect_system=1` and `protect_home` on for every service that doesn't set them. An explicit `key=0` opts a service out. The kernel-surface knobs above are never defaulted; an explicit one that would hide `exec` or `ready_path` refuses the load. A defaulted `private_tmp` or `protect_home` that would hide the service's own `exec` or `ready_path` is dropped with a log line; an explicit one refuses the load. The switch is off unless you turn it on. `schema-ctl status <svc>` shows each knob's value and whether it came from the file, the default, or was dropped.

A hardening step that fails aborts the spawn (`HARDENING FAILED` in the service log) rather than running the service unconfined.

## Resource limits from systemd units

`schema-import` maps a unit's `MemoryMax=` (or the older `MemoryLimit=`) to `mem_limit` (rounded up to whole MB), `MemoryLow=`/`MemoryMin=` to `mem_low` (the larger; `memory.min` itself has no key), `MemorySwapMax=` to `swap_max` (a slice's `MemorySwapMax=` is a ceiling and the tightest wins, like `MemoryMax=`; a slice's `MemoryLow=`/`MemoryMin=` protects the slice as a whole, has no equivalent per member, and is warned about), `CPUQuota=` to `cpu_limit`, `CPUWeight=` to `cpu_weight` (`idle` → `1`) and `TasksMax=` to `pids_max`. `infinity` or an empty value means no key. A percentage `MemoryMax=`/`TasksMax=` has no equivalent and is warned about, as are `MemoryHigh=`, `IOWeight=`, `AllowedCPUs=` and the other resource directives with no key here.

There are no slice objects. A unit's `Slice=` is resolved through the same unit directories and drop-ins as the unit itself, together with every parent slice (`a-b.slice` sits in `a.slice`). Each cap they set is folded onto the member, and the tightest of the unit's own value and the slices' values wins. `CPUWeight=` is relative, not a cap, so the unit's own value is used, or else the nearest slice's. Under systemd a slice caps its members **together**. Here each member gets the whole ceiling, and the generated file warns about this. Slices with no limits (Fedora's `system-cups.slice` and the like) are ignored, as are the root slices (`system.slice`, `user.slice`, `app.slice`, …). `--user` works the same way, using the user unit directories.

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

**Wall-clock timers** — set `on_calendar=HH:MM` to fire at a fixed local time every day, `on_calendar=Mon HH:MM` for weekly, or `on_calendar=15 HH:MM` for a day of the month, the way you'd write a cron line. A comma list of hours fires at each one: `on_calendar=6,18:00` runs at 06:00 and 18:00. This is the form you want for "3am backup", "midnight log rotation", "nightly cert renewal":

```ini
name=nightly-backup
exec=/usr/local/bin/backup.sh
needs_root=1
on_calendar=03:00     # fire at 03:00 local time, every day
```

`on_calendar` re-evaluates the wall clock on every fire, so it tracks `CLOCK_REALTIME` (not monotonic) — DST shifts and NTP clock steps self-correct each cycle rather than drifting. Time is local (`/etc/localtime`). A malformed value refuses the service (it would otherwise run once at boot as a plain daemon); an empty `on_calendar=` is ignored. `on_calendar=Mon..Fri 09:00`, `on_calendar=*:0/15` and `on_calendar=Sun 1..7 01:00` (first Sunday of the month) are all one line each; seconds are always `:00`, and there is no month field.

**Spreading the load:** `randomized_delay_sec=3600` on a midnight timer starts it somewhere between 00:00 and 01:00, a new draw every night; it applies to interval timers and a persistent catch-up too. `schema-ctl status <svc>` shows the next fire. `on_active_sec` already counts from completion, which is systemd's `OnUnitInactiveSec=`.

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

## Dynamic users

`dynamic_user=1` runs a service as a uid nobody else has, picked from systemd's range 61184–65519: hashed from the service name, so it is the same every boot unless taken, and skipping any uid or gid in `/etc/passwd`/`/etc/group` or held by another service. The gid is the same number. It keeps that uid across `schema-ctl reload` and PID 1 re-execs; `schema-ctl status <svc>` shows it. Inside, `USER`/`LOGNAME` are the service name and `HOME=/`. There is no NSS module: `getpwuid()` returns nothing for the uid (systemd ships nss-systemd for this).

It turns on `no_new_privs`, `private_tmp`, `protect_system=full` and `protect_home` unless the service sets them. Its `state_dir`/`cache_dir`/`logs_dir` live under a root-only `private/` (`/var/lib/private/<dir>`, mode 0700), with `/var/lib/<dir>` a symlink to it; in the service's own mount namespace `private/` holds only its dirs, so a service handed the same uid later can't read them. An existing plain `/var/lib/<dir>` is moved in; a dir left owned by an old uid is re-owned without following symlinks or crossing mounts. These dirs take one path component (`state_dir=foo`, not `foo/bar`). `RemoveIPC=` and `RestrictSUIDSGID=` are not implemented yet.

## Syscall filtering

`syscall_filter=` and its siblings build a seccomp-BPF program for the service by hand (PID 1 is static, so no libseccomp) and install it as the very last step before `execve`, in the main process and in the `exec_stop`/`exec_reload` runners. `execve` itself is always allowed. The program checks the architecture first: a syscall from any other ABI (on x86_64 that includes x32 and 32-bit compat calls) kills the process, so nothing gets around the filter that way, and 32-bit binaries can't run under a filtered service. Allow/deny lists are searched as a balanced tree (about 9 compares for `@system-service`, not 400).

**Blocked calls return EPERM by default.** systemd kills the process instead; here a failure shows up in the service's log as an error. Set `syscall_error_number=kill` for systemd's behaviour, or `:ERRNO` on one entry. Without `CAP_SYS_ADMIN` (any service with `user=`), the kernel needs `no_new_privs`, which is then set for it.

The group tables come from `systemd-analyze syscall-filter` (systemd 259) via `tools/gen-seccomp-tables.py`; syscall numbers come from each architecture's kernel headers at build time.

```ini
syscall_filter=@system-service
syscall_filter=~@privileged @resources
restrict_suid_sgid=1
lock_personality=1
```

## Path watches

`watch_path=` starts a service from the filesystem, the way a systemd `.path` unit starts its service. The service is armed at boot (state `PERFECT`, counted as up by dependents) and runs each time a watch fires, then is armed again whatever its exit code:

```ini
name=spool-drain
exec=/usr/local/bin/drain
watch_path=dir_not_empty:/var/spool/drain
make_directory=1
```

Watches use PID 1's inotify descriptor (schema-user's for user services). A path that doesn't exist yet is watched from its nearest existing parent and followed as directories appear; parents are followed through symlinks (`/var/run` is watched as `/run`). Every 30 s the watches are also re-armed and re-checked, for filesystems inotify can't see (bind mounts, NFS). The rail logs `path-fire KIND:PATH`, `path-done`/`path-failed` and `path-trigger-limit`; `schema-ctl status <svc>` lists the watches and the last fire. `schema-ctl stop` parks the service; `start` runs it now and arms it again.

`schema-import` folds a `.path` unit and the service it starts into one `.svc` named after the path unit. It skips a `.path` whose service is socket- or timer-activated, one using `%v`, and `systemd-ask-password-*.path`.

---

## Rootless podman / Quadlet

`schema-import --user` runs Quadlet (and any other user generator), so a `.container` file in `~/.config/containers/systemd/` becomes a schema-user service with no extra step; `systemctl --user daemon-reload` picks up edits (then `restart` the service to apply them, as on systemd). With lingering on, it starts at boot before anyone logs in. The generated `.svc` uses `main_pid_notify` (supervises conmon after `podman run -d` exits), `exec_stop`/`exec_stop_post` (the container is removed on stop), `verify_*_max` (a `Memory=`/`--cpus`/`--pids-limit` that did not land fails the service) and a `stop_timeout_sec` past podman's own stop timeout. Rootless ports are ≥ 1024. `podman exec` works from a login shell: podman asks the session bus for its own `podman-<pid>.scope`, the session shim creates it under `user@U.service/user.slice/`, and the root bridge (`AttachProcessesToUnit`, own processes of the caller's uid only) moves podman there, so crun can move the exec into the container's cgroup. Other transient scopes are still refused. Needs a session bus; `tests/livetest/podman-exec-scope.sh` checks it. `tests/livetest/podman-quadlet-e2e.sh` is the end-to-end check.
