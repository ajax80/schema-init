# Running a schema-init box

## Runtime control

`schema-ctl` is a control client that communicates with the running init over a Unix domain socket at `/run/schema-init.sock`.

```sh
sudo schema-ctl status          # full state dump for all services
sudo schema-ctl status --json   # machine-parseable JSON — for supervisory loops and IEC 62304 audit
sudo schema-ctl status --kv     # flat key=value — grep-friendly
sudo schema-ctl status <name>   # one service: state, last exit, readiness, each hardening knob and its source
sudo schema-ctl timing          # per-service spawn→ready cost, boot critical path first
sudo schema-ctl analyze         # boot critical chain + waterfall; how each service proved ready (notify / bus-name / ready_path / stable timer / exit)
sudo schema-ctl analyze <name>  # the dependency chain that held <name> back
sudo schema-ctl list            # names and current states only
sudo schema-ctl cat <name>      # the .svc file and its drop-ins, in apply order
schema-ctl graph [<name>] | dot -Tsvg > deps.svg   # dependency graph (all, or <name>'s chain); missing deps in red
sudo schema-ctl start <name>    # start a stopped or EXCISED service
sudo schema-ctl stop <name>     # send SIGTERM to a running service
sudo schema-ctl restart <name>  # stop + re-queue through the state machine
sudo schema-ctl add <svc|path>  # load a new .svc at runtime (bare name = /etc/schema-init/services/<name>.svc)
sudo schema-ctl reload          # re-read the services directory (rejected if new config has a cycle)
sudo schema-ctl reload --evict  # reload + SIGTERM any running service no longer present in config
sudo schema-ctl pet <name>      # service heartbeat check-in — resets watchdog_timeout_ms window
sudo schema-ctl reset [<name>]  # reset restart/dormant counts and re-queue failed services
sudo schema-ctl reexec [<path>] # replace the PID 1 binary in place, services keep running
sudo schema-ctl reboot          # orderly shutdown sweep, then reboot (also: poweroff)
```

**`reload --evict`** sends SIGTERM to each running service that is no longer in the config, then gives it 3 seconds. Whatever is still in its cgroup after that is killed with `cgroup.kill`, including children left behind by a main process that did exit. A service that exits on SIGTERM in time is not reported as killed. Once the cgroup is empty its directory is removed. If a later `reload` or `reexec` brings the same service name back within those 3 seconds, the old instance's leftovers are killed first, so the new instance starts in a clean cgroup and is not caught by the old deadline. A task still in the cgroup 5 seconds after the kill (stuck in D state) is logged once, and the directory is removed when it finally dies. Without `--evict`, removed services keep running unmanaged until they exit on their own.

`schema-ctl` exits 1 when PID 1 refuses a command (`err:` reply), so scripts can check it. The socket is `chmod 0600` — root only. Build alongside the init binary:

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

Note that this lets anyone at the physical console restart a service. The shipped gettys ask for a
password, so this is a new privilege for whoever can reach the keyboard — worth knowing before you
enable it on a machine other people can walk up to.

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

**Rescue boot.** Add `schema.rescue` (or `single`, `s`, `S`, `1`, `rescue`) to
the kernel command line; on Devuan/LCOS pick the **schema-init: … (recovery
mode)** GRUB entry. PID 1 mounts `/proc`, `/sys`, `/dev`, `/run`, makes `/`
writable and runs `sulogin` on the console before any service starts: no udev,
no network, nothing from `/etc/fstab` (`mount -a`). Exit the shell and the boot
continues normally. sulogin asks for root's password; with the root account
locked (Fedora's default) it gives no shell and the boot goes on, so use
`init=/bin/sh` there.

**fsck.** `mount-fstab` checks each `/etc/fstab` filesystem with a pass number
above 0 (`fsck -a`) before mounting it; `/` is the initramfs's. FAT filesystems are
skipped when the boot-sector dirty bit and the FAT[1] clean-shutdown bits say
they were unmounted cleanly, unless forced. When fsck
cannot fix one, a `nofail` entry is left unmounted and any other gets the same
sulogin shell; after it, the filesystem is mounted if fsck passes, otherwise
left out. `forcefsck` / `fsck.mode=force` on the command line or `/forcefsck`
force the check, `fastboot` / `fsck.mode=skip` or `/fastboot` skip it; both files count for one boot.

**Dependency cycle.** When the service graph has a cycle, PID 1 starts no
services and runs `/bin/sh` on tty1 without a password. Exiting it halts the
machine. Fix the `.svc` files, then reboot:

```sh
ls /etc/schema-init/services/
vi /etc/schema-init/services/broken.svc
exit
```

---

## Flight recorder

`schema-recorder` is an opt-in service that answers "what just happened" when nothing looks failed. It keeps a rolling record of service events, CPU and memory pressure (read from `/proc`), and kills reported to it, in `/var/lib/schema-init/` (root-only, `0600`). It is not started by default:

```sh
sudo cp /usr/share/schema-init/services/schema-recorder.svc.example /etc/schema-init/services/schema-recorder.svc
sudo schema-ctl add schema-recorder
```

The board shows the latest event on its arrow line. `schema-ctl trace` prints the history; `--since <epoch|HH:MM>` limits it.

Other programs report to it with `schema-recorder send src=NAME kind=KIND msg=TEXT`. For earlyoom, add `-N /usr/local/bin/schema-recorder-earlyoom` to its arguments and each kill is recorded with the pid, name and command line.

---

## Descriptor store (fdstore)

A service with `fdstore=N` in its `.svc` file (0 to 4096, default 0 = off) can hand PID 1 file descriptors to keep across its own restarts. It sends them over `NOTIFY_SOCKET` with `FDSTORE=1` and an `FDNAME=` label; PID 1 holds them and passes them back on the next start, after any `listen=` sockets, through `LISTEN_FDS` and `LISTEN_FDNAMES`. `FDSTOREREMOVE=1` with an `FDNAME=` removes them again. PID 1 never reads or polls a stored descriptor.

| Event | Stored fds |
|---|---|
| `schema-ctl restart`, crash and automatic restart | kept |
| PID 1 re-exec (`dnf upgrade`), config reload | kept |
| `schema-ctl stop`, dormant, excised, removed from config | dropped |
| System shutdown | dropped |

`schema-ctl status` shows `fds=K` for a service holding K descriptors.

**schema-dbus uses it.** On SIGTERM the broker stops taking new work, drains for up to 2000 ms, snapshots its state into a memfd, and stores its listen sockets, the snapshot, and every client connection with its pending fds. Its `dbus.svc` sets `fdstore=1024`, room for every system-bus connection plus the listen and state descriptors. The next start resumes from them, so `sudo schema-ctl restart dbus` keeps every client connected. A client that has not read for 2 s while the broker drains is dropped alone. If the snapshot is unreadable, or PID 1 refused part of the store, the broker starts fresh and clients reconnect, as before.

To update the broker live by hand, install the new package, then restart it:

```sh
sudo dnf upgrade schema-init
sudo schema-ctl restart dbus
```

The package does not restart the broker for you yet. The fallback launcher (`schema-dbus-run.sh`) clears `LISTEN_*` and `NOTIFY_SOCKET` and closes the stored descriptors before it runs stock `dbus-daemon`, which would otherwise inherit them. That needs python3 or perl; with neither, only descriptors 3 to 9 are closed.

---

## Per-user supervisor (schema-user)

`schema-user` runs the same engine as PID 1, as an ordinary user, for that user's own services. It refuses to run as root.

**Started by logind.** schema-logind starts one `schema-user` for you at your first session and stops it 30 s after your last session closes (a new session inside those 30 s, such as an autologin desktop restarting, keeps it running). Your desktop session never waits for it: if it fails to start or keeps crashing, the login goes on as before.

- **Restarts:** if it exits on its own, logind starts it again after 1, 2, 4, 8, 16, 32, then 60 s. After 5 crashes within 5 minutes it gives up until your next login, and says so in its log.
- **Linger:** `loginctl enable-linger` (as yourself, from a session, or as root for anyone) starts it at boot and keeps it running with no session; `loginctl disable-linger` undoes that. The flag is `/var/lib/systemd/linger/<user>`.
- **logind restarts:** a `kill -HUP` re-exec of schema-logind, or a stop and start, picks up the running instance instead of starting another. Your services never restart because logind did.
- **Shutdown:** PID 1 sends it SIGTERM along with the containers, so your services get their ordered stop before the final kill.
- **Kill switch:** create `/etc/schema-init/user-supervisor.disabled`, or add `schema.user=0` to the kernel command line at the boot menu, and logind starts no `schema-user` at all. An instance already running is left alone.

| What | Where |
|---|---|
| Service files | `$XDG_RUNTIME_DIR/schema-user/services/*.svc` (same syntax as PID 1's) |
| Control socket | `$XDG_RUNTIME_DIR/schema-user/ctl.sock`, mode 0600 |
| Notify socket | `$XDG_RUNTIME_DIR/schema-user/notify` |
| Service and rail logs | `$XDG_STATE_HOME/schema-user/log/` (default `~/.local/state/schema-user/log/`) |
| Persistent timer stamps | `$XDG_STATE_HOME/schema-user/timers/` |
| Cgroup (delegated to you) | `/sys/fs/cgroup/user.slice/user-<uid>.slice/user@<uid>.service/` |
| Its own output | `/var/log/schema-init/schema-user-<uid>.log` (root-readable) |

Drive it with `schema-ctl --user`, e.g. `schema-ctl --user status` or `schema-ctl --user restart <svc>`.

- `XDG_RUNTIME_DIR` must be set and be a directory you own, or it exits 1. A second copy for the same user exits 1 with "already running".
- Services start with `HOME`, `USER`, `LOGNAME`, `SHELL`, `PATH`, `XDG_RUNTIME_DIR` and `DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus`, plus `KEY=VALUE` lines from `*.conf` in `/usr/lib/environment.d`, `/etc/environment.d` and `$XDG_CONFIG_HOME/environment.d`, in that order (later wins). `$VAR` and `${VAR}` are expanded; nothing else is. The environment `schema-user` itself was started with is not passed on, so `WAYLAND_DISPLAY` and `DISPLAY` never leak in.
- Run by hand, it gives each service its own cgroup only if the cgroup it was started in was delegated to you (you can write its `cgroup.procs` and create directories in it). It moves itself into `init.scope` there. Otherwise it says so once and runs without per-service cgroups.
- Without a delegated cgroup, `notify=` (and so `watchdog_sec=`) and `ready_bus_name=` cannot attribute a message to a service and fall back to `ready_path`/`stable_secs`, and a `pid_file=` service is excised. Started by logind, it has the delegation unless setting it up failed (logged in schema-logind.log).
- It is a child subreaper: a service's double-forked daemon stays its child. On SIGTERM or SIGINT it stops services in reverse dependency order, kills anything still reparented to it, and exits 0.
- `wait_socket=<path>` in a `.svc` holds a service until a connection to that `AF_UNIX` socket succeeds (a socket file left by a dead listener does not count). Point session services at `$XDG_RUNTIME_DIR/bus` to start them once the session bus is up; `schema-ctl --user status` shows `waiting=<path>` meanwhile.

---

## Crashes

Fedora's initrd points `kernel.core_pattern` at `systemd-coredump`, which hands each core to a socket-activated `systemd-coredump.socket` that does not exist under schema-init, so every crash is logged as "Failed to connect to coredump service" and the core is thrown away. At boot PID 1 replaces **that** pattern, or the kernel's default `core` when the initrd sets none, with `schema-coredump` (apport, abrt or a pattern you set by hand is left alone), then raises its soft `RLIMIT_CORE` to the hard limit so services inherit a limit the helper can honour.

Each crash gets a record in `/var/lib/schema-coredump/` (root-only, `0700`): `core.<comm>.<uid>.<time>.<pid>.meta` with the pid, signal, exe, command line, cgroup and schema-init service, and beside it a zstd core unless something says not to:

| RESULT | meaning |
|---|---|
| `stored` | core kept |
| `rlimit` | the process ran with `RLIMIT_CORE` below 4 KiB; it opted out |
| `rate-limited` | the same program already left a core in the last 60 s (crash loop) |
| `no-space` | the filesystem is within 10% of full |
| `truncated` | the core hit a cap: `RLIMIT_CORE`, 2 GiB uncompressed, or the space budget |

Old cores are removed oldest-first once they total 4 GiB; 200 records are kept. One line per crash goes to syslog (`/var/log/schema-init/journal.log`).

```sh
sudo schema-coredump --list
sudo zstd -dc /var/lib/schema-coredump/core.foo.….zst > core && gdb /usr/bin/foo core
```

If something re-applies `sysctl.d` after boot (a `sysctl --system` service), it puts systemd-coredump back; run `schema-coredump --take-pattern` after it.

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

**It is scheduled out of the box.** There is no cron and no `logrotate.timer` here, so rotation runs as an ordinary wall-clock timer, `logrotate.svc`:

```ini
name=logrotate
exec=/usr/sbin/logrotate
args=/etc/logrotate.conf
condition=path_exists:/usr/sbin/logrotate
needs_root=1
on_calendar=00:10
persistent=1
```

How it gets there depends on the install path:

- **ISO install, or `schema-migrate --deploy` / the wizard** — Fedora enables `logrotate.timer`, and the importer translates every enabled unit, so `/etc/schema-init/services/logrotate.svc` is written for you.
- **`make install` from a clone** — installs `services/logrotate.svc` into `/etc/schema-init/services/` unless one is already there. A staged install (`DESTDIR` set, as in the RPM build) leaves `/etc/schema-init/services/` alone.

If logrotate isn't installed, the condition skips the timer instead of failing it.

`persistent=1` matters here: a box that is powered off at 00:10 would otherwise skip that day's rotation entirely and only catch up the next night.

Remove that file (or never have one) and the config sits inert and logs grow unbounded. `maxsize 100M` is not a safety net on its own — it is only consulted when logrotate actually runs.

**`journalctl` shim (optional Track B)** — software and post-install scripts that shell out to `journalctl -u <svc>` would fail with no journald present. `scripts/journalctl` is a drop-in interceptor: install it to `/usr/local/bin/journalctl` and it serves the matching `*.log` from the directories above, swallows unknown flags, supports `-o json`, and always exits 0 so a caller piping it to `jq`/`awk` never hard-crashes. It does **not** read a binary journal — there isn't one.
