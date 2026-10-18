# schema-init compared

How schema-init stands next to the other Linux inits: what each one is built
from, and, where this project has measured it, how fast and how heavy it is
next to schema-init on the same install. Where an init has not been measured,
this says so instead of guessing.

## What each one is

| | PID 1 and supervision | Service definitions | Dependencies and readiness | Logging | Mainly on |
|---|---|---|---|---|---|
| **systemd** | one PID 1 that starts, supervises and tracks everything in cgroups | unit files (INI) | `Requires=`/`After=`; readiness by `sd_notify`, D-Bus name, socket or fork | journald (binary journal) | most distros |
| **sysvinit** | `init` reads `/etc/inittab`, respawns gettys; services are not supervised | shell init scripts | run in a fixed order (LSB headers, `insserv`, `startpar`); a script's exit is "ready" | syslog | Devuan, Slackware |
| **OpenRC** | runs on sysvinit's `init` or its own `openrc-init`; supervision optional (`supervise-daemon`) | shell scripts with `depend()` | `need`/`use`/`after`; ready when the start function returns | syslog | Gentoo, Alpine, Artix |
| **runit** | `runit` PID 1, `runsvdir` + one `runsv` per service | a `run` script per service | none; a `run` script checks its own (`sv check`) and exits to be retried | `svlogd` per service | Void, Artix |
| **s6** (s6-linux-init, s6-rc) | `s6-svscan` as PID 1, one `s6-supervise` per service | a directory per service, compiled into a database (`s6-rc`) | `dependencies`; ready by a notification fd | `s6-log` per service | Artix, Obarun (66) |
| **dinit** | one PID 1 that starts and supervises | INI-like service files | `depends-on`/`waits-for`/`before`/`after`; ready by a pipe fd or the process starting | to file or a buffer, per service | Chimera, Artix |
| **GNU Shepherd** | one PID 1 written in Guile Scheme | Scheme | `requirement`; ready when the start procedure returns | to file | Guix System |
| **BusyBox init** | `/etc/inittab`, respawn only | one rc script | none | syslog | embedded, initramfs |
| **schema-init** | one static C PID 1, single thread, supervises in cgroups | `key=value` `.svc` files | `dep=`; ready by `ready_path`, `sd_notify`, a D-Bus name or a PID file, else `stable_secs` | plain text per service, `journal-sink` for journald clients | Fedora (packaged), Devuan, Void, Artix (from source) |

Two things set schema-init apart from the lean inits (sysvinit, runit, s6,
dinit). It replaces the daemons around PID 1 on a systemd distro, not just
PID 1: `schema-logind`, `schema-udev`, `schema-dbus`, `journal-sink` and timers
stand in for logind, udevd, dbus-broker, journald and cron, so a Fedora KDE
desktop runs without systemd's stack. And it reads systemd units: `schema-import`
turns `.service` files into `.svc` files, so Fedora's packages run unmodified.
The lean inits run on distros that already dropped systemd and wrote their own
scripts.

What the others have that schema-init does not:

- **Age.** sysvinit and runit have decades of use, OpenRC, s6 and dinit about
  ten years each, and systemd runs most of the Linux world. schema-init was
  first booted in 2026, and `schema-dbus` and `schema-logind` have had no
  outside security review.
- **systemd's breadth**: user sessions (`systemd --user`), `systemd-homed`,
  `networkd`, portable services, `nspawn`. schema-init replaces what a desktop
  needs, not all of it.
- **s6's rigour**: readiness by fd for any daemon that can write one, and the
  pipe between a service and its logger held open by the supervisor, so either
  can restart without losing output.
- **runit's size**: a few hundred lines per program, and nothing to configure.

## Measured

Every number below comes from one install booted both ways, its stock init and
schema-init, alternating, with the same kernel, packages and desktop on both
sides. Each table links its raw data. None of these were measured on the same
machine as another, so compare columns within a table, not across tables.

### Desktop boot: time to the drawn desktop

| Rival | Distro, desktop, machine | Rival | schema-init | Difference |
|---|---|---|---|---|
| systemd | Fedora 44 KDE, laptop (Celeron N4000, HDD), from kernel start | 98.6 s | **71.1 s** | 27.5 s sooner |
| sysvinit | LCOS 0.9.3 (Devuan) Xfce, same laptop, from kernel start | 27.35 s | **25.35 s** | 2.0 s sooner |
| runit | Void glibc Xfce, VM, from switch-root | 6.22 s | **5.75 s** | 0.47 s sooner |
| dinit | Artix Xfce, VM, from root mount | 4.21 s | **2.89 s** | 1.32 s sooner |
| s6 (s6-rc) | Artix Xfce, VM, from root mount | 2.75 s | **2.48 s** | 0.27 s sooner |
| OpenRC | Artix Xfce, VM, from root mount | 7.04 s | **2.79 s** | 4.25 s sooner |

### Memory and processes after boot

| Rival | RAM used: rival | schema-init | Processes: rival | schema-init | PID 1 RSS: rival | schema-init |
|---|---|---|---|---|---|---|
| systemd (Fedora KDE) | 1872 MB | **1433 MB** | 239 | **207** | — | — |
| sysvinit (LCOS Xfce) | **427 MB** | 432 MB | 150 | 150 | — | — |
| runit (Void Xfce) | **452 MB** | 460 MB | 174 | **157** | — | — |
| dinit (Artix Xfce) | **507 MB** | 512 MB | **154** | 156 | 4.5 MB | **1.6 MB** |
| s6 (Artix Xfce) | 518 MB | **510 MB** | 192 | **156** | 2.2 MB | **1.6 MB** |
| OpenRC (Artix Xfce) | 513 MB | **512 MB** | 164 | **155** | 2.0 MB | **1.6 MB** |

Against systemd the gain is large, because schema-init replaces journald,
logind, udevd and the D-Bus broker as well as PID 1. Against the lean inits
there is little left to take: RAM is within 10 MB either way, and schema-init
is sometimes the heavier one.

### Where it is behind

- **s6 starts sshd and NetworkManager sooner**: 0.10 s and 0.08 s after
  root mount, against 0.20 s and 0.23 s. Until 2026-10-18 schema-init held
  NetworkManager until udev coldplug had settled (0.60 s); it now starts it
  once the coldplug events are queued, as s6 and systemd do. The rest of the
  gap is PID 1's own order: udevd, then `mount-fstab` and `artix-boot`, before
  sshd. The desktop still comes up sooner under schema-init.
- **runit, sysvinit and dinit used less RAM** after boot in these runs, by 5
  to 8 MB, but the spread between boots of the same init is wider (dinit
  503–514 MB, schema-init 501–518 MB): Xorg and the window manager alone vary
  by up to 18 MB from one boot to the next. On a pair of dinit and schema-init
  boots compared process by process, schema-init's own processes (PID 1 and
  `journal-sink`) took about 1 MB more than dinit's PID 1.
- **dinit runs two fewer processes** than schema-init on Artix.

### Lost device events at boot

Artix's s6 and OpenRC packages start udevd in the background and run
`udevadm trigger` right after, without waiting for udevd to be ready. If the
trigger wins the race, the coldplug events are lost: no network driver, no
seat for the graphics card, so no network and no desktop (the login prompt on
tty1 still works). In these VMs:

- **OpenRC**: 3 of the 5 stock boots while setting up the VM, 1 of 9 in the
  first benchmark run, 0 of 9 in the second. Its `udev` script starts udevd
  with `command_background=yes` and `udev-trigger` only `need`s it.
- **s6**: 2 of 10 recorded boots on a disk whose fstab had no entries (a
  mistake in our disk builder), and the first two boots after install; 0 of 20
  with a normal fstab. On a failed boot udevd processed 18 events where a
  manual re-trigger processed about 2,600.

dinit does not have this problem: it starts udevd with `--daemon`, which
returns only once udevd is ready. schema-init had a window too: its udevd
service promoted on `/run/udev/control`, most likely a moment before udevd
listens for device events, and one of its first dozen boots on Artix came up
without network the same way. Its coldplug now runs `udevadm control --ping` before
triggering, retried for up to 30 s. Since then, one boot of about 100 came up
without network: during the first s6 run on 2026-10-18, while blakbox itself
was stalled for memory, alongside three failed boots of stock s6 in the same
half hour. 35 boots repeated on the same disk the same day, with a log of the
ping and the coldplug events on each, had no failures. eudev (Void, Devuan)
has no `--ping`, and needs none: its udevd starts listening for device
events before it creates its control socket. For s6, adding a
`notification-fd` and `s6-notifyoncheck -c "udevadm control --ping"` to the
udevd service closed the window (0 failures in 9 boots,
[raw](../tests/livetest/artix-vm-bench-20261017-s6-notify.txt)), and drew the desktop
about 0.3 s later than stock s6 on the same disk (3.11 s against 2.79 s,
measured while that disk's fstab was still empty, so not comparable with the
table above).

### Method

**Artix** (dinit, s6, OpenRC): one VM disk per init, built the same way from
Artix's repositories on 2026-10-17 (kernel 7.2.10, udev 262, dinit 0.22.1,
s6-rc 0.7.0 with s6-base 3.3.3, OpenRC 0.64.1): `base`, Xfce 4.20, LightDM
with autologin, NetworkManager, sshd, and only the init's own service packages
differing. schema-init added with
[`distros/artix/install.sh`](../distros/artix/README.md). QEMU/KVM on blakbox,
4 vCPU, 6 GB, virtio disk. 9 reboots per side, alternating, readings 60 s
after boot by [`tests/livetest/artix-metrics`](../tests/livetest/artix-metrics),
first run discarded, medians of 8. Times are seconds from the kernel mounting
the root filesystem (the initramfs hands over straight after), so kernel and
initramfs time, the same code on both sides, is left out. Raw data:
[dinit](../tests/livetest/artix-vm-bench-20261017-dinit.txt),
[s6](../tests/livetest/artix-vm-bench-20261017-s6.txt),
[s6 stock boots](../tests/livetest/artix-vm-bench-20261017-s6-stock-boots.txt),
[s6 on 2026-10-18](../tests/livetest/artix-vm-bench-20261018-s6.txt) (a first run that day,
taken while blakbox was short of memory and its VMs stalled, is kept
[here](../tests/livetest/artix-vm-bench-20261018-s6-hoststall.txt) and not used),
[OpenRC](../tests/livetest/artix-vm-bench-20261017-openrc.txt).

The OpenRC VM ran with QEMU's HPET off (`-machine q35,hpet=off`). With it on,
reading the emulated RTC timed out after 10 s, and OpenRC is the only one of
these inits whose boot reads the RTC (its `hwclock` service; the others set the
time zone with `hwclock --systz` and leave the clock to the kernel). That run
had OpenRC at 16.8 s to the desktop, an artifact of the VM, so it is not used
([raw](../tests/livetest/artix-vm-bench-20261017-openrc-hpet.txt)). With HPET
off the read takes under a second, as on hardware, and that second stays in
OpenRC's numbers. OpenRC also starts services one at a time: Artix leaves
`rc_parallel` at its default, off.

systemd, sysvinit and runit: see [Numbers](benchmarks.md) for each method and
its raw data.

### Not measured

GNU Shepherd, BusyBox init, 66 and s6 outside Artix have not been run against
schema-init. Neither has any rival on real hardware other than systemd and
sysvinit.
