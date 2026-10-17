# schema-init on Void Linux (beside runit)

Boot Void Linux with schema-init as PID 1, side by side with runit. Nothing
about your runit boot changes: it stays the default GRUB entry, and
schema-init gets its own entries below it. Pick one at boot to try it; pick the
normal entry to go back.

Tested in a UEFI VM on **Void glibc** (rootfs 20250202, updated October 2026:
kernel 6.18, runit-void 20250212), headless and with Xfce 4.20 + LightDM +
elogind. On each: boots under schema-init, the services you enabled for runit
run, and Xfce's Restart and Shut Down work. The runit entry still boots and
shuts down normally with this installed. **musl is not tested yet.** Not yet
run on real hardware.

## Compared with runit

Measured in the same VM (4 vCPU, virtio disk), 4 boots each, alternating,
timed from the moment PID 1 takes over (dracut's switch-root), medians:

| | runit | schema-init |
|---|---|---|
| Headless: sshd started | 1.34 s | 1.29 s |
| Xfce: Xorg started | 1.79 s | 1.45 s |
| Xfce: session started | 2.18 s | 1.81 s |
| Xfce: desktop drawn (xfdesktop) | 6.22 s | 5.75 s |
| RAM used after boot (Xfce) | 452 MB | 460 MB |
| Processes after boot (Xfce) | 174 | 157 |

Most of the desktop difference is ordering: Void's `lightdm` run script exits
when `sv check dbus` fails, and runsv retries it a second later. schema-init
starts LightDM once the D-Bus socket exists. schema-init uses about 8 MB more
memory; not yet investigated.

What you get:

- **Start on ready, not on retry.** A service starts once the ones it waits
  for are up. The `sv check dbus || exit 1` lines in run scripts become
  dependencies instead of exit-and-retry loops.
- **Ordered shutdown.** Services stop in reverse dependency order, then what
  is left is killed and filesystems go read-only (Xfce VM: services down in
  about 0.2 s).
- **A log per service** in `/var/log/schema-init/`, without an svlogd per
  service: one process per service instead of runsv + the daemon + its logger.
- **Boot timing**: `schema-ctl analyze` shows the critical chain and how each
  service proved ready.
- **Live control**: `schema-ctl status`, `start`, `stop`, `add`, and
  `schema-ctl reexec` to replace PID 1 in place. `sv` still works for the
  common commands (below).
- **A rescue boot and fsck at boot**, timers without cron, and per-service
  sandboxing, as on the other distros.

What it costs:

- Your runit services run unmodified, but schema-init runs their `run`
  scripts itself. runit's `log/` loggers, `finish` scripts and runlevels other
  than `default` are not used. A `check` script is translated only for dbus;
  any other service with one is treated as ready one second after it starts
  (the `.svc` says so).
- No xbps hook: after enabling a service, run the import command below (or
  reboot into runit and back).
- runit has two decades of use behind it. This path was first tested in
  October 2026.

## Install

```sh
sudo xbps-install -S git make gcc pkg-config acl-devel dbus-devel libzstd-devel python3
git clone https://github.com/ajax80/schema-init.git
cd schema-init
make
sudo distros/void/install.sh
```

Then reboot and choose **schema-init: Void GNU/Linux, with Linux …** in the
GRUB menu.

Check it from a terminal:

```sh
sudo readlink /proc/1/exe     # /usr/bin/schema-init
sudo schema-ctl status        # every service FUNDAMENTAL or PERFECT
```

To remove the boot entries: `sudo distros/void/install.sh --remove`.

## What install.sh does

- `make install`: binaries to `/usr/bin` (`schema-init`, `schema-ctl`,
  `schema-journal-sink`, ...).
- Copies the service files in `services/` to `/etc/schema-init/services/`,
  keeping any you have edited (it prints which), and the boot helpers in
  `scripts/` to `/usr/local/libexec/schema-init/`.
- Runs `schema-import --runit`: every service linked in
  `/etc/runit/runsvdir/default` (what `/var/service` points to) gets a `.svc`.
- Installs an `sv` wrapper in `/usr/local/bin` (see [sv](#sv)).
- Installs `/etc/grub.d/11_schema-init` and runs `grub-mkconfig`. That script
  runs GRUB's own `10_linux` with `init=/usr/bin/schema-init` added, so new
  kernels get a schema-init entry automatically.

## Services

| Service | Replaces (runit stage 1) | Runs |
|---|---|---|
| `udevd`, `udev-coldplug` | `02-udev.sh` | `udevd`, then `udevadm trigger` + `settle` |
| `mount-fstab` | `03-filesystems.sh`, `04-swap.sh` | fsck and mount `/etc/fstab`, swap |
| `void-boot` | the rest of `/etc/runit/core-services` | Void's own scripts: modules, console, hostname, seedrng, sysctl, dmesg, wtmp |
| `journal-sink` | — | owns `/dev/log`; logs to `/var/log/schema-init/journal.log` |

PID 1 itself mounts `/proc`, `/sys`, `/dev`, `/run`, cgroup2, and remounts `/`
read-write (`00-pseudofs.sh`).

`void-boot` sources every script in `/etc/runit/core-services` except the four
above, so a package that adds one (and Void's own updates to them) runs under
schema-init too.

### Your runit services

Each enabled service becomes a `.svc` that runs `cd /etc/sv/<name> && exec
./run`, the way runsv does, and restarts it when it dies:

```ini
# schema-import: from /etc/sv/lightdm (runit)
name=lightdm
exec=/bin/sh
args=-c
args=cd /etc/sv/lightdm && exec ./run
needs_root=1
stable_secs=0
dep=void-boot
dep=dbus
```

`dep=dbus` comes from the script's `sv check dbus` line. A link with a `down`
file is skipped, as runit leaves it down. `void-covered` lists the services
`distros/void` runs itself (`udevd`).

Enabling a service works as on runit (`/var/service` still resolves under
schema-init); then load it:

```sh
sudo ln -s /etc/sv/<name> /var/service/
sudo /usr/local/libexec/schema-init/schema-import --runit --after void-boot \
     --rc-skip /usr/local/libexec/schema-init/void-covered --start
```

`--start` starts it right away when schema-init is PID 1.

### sv

`/usr/local/bin/sv` hands everything to runit's `sv` when runit is PID 1.
Under schema-init it maps `status`, `check`, `up`/`start`, `down`/`stop`/`exit`
and `restart`/`term` to `schema-ctl`; other commands print "not supported".

## Shutdown

runit's own `halt`, `reboot`, `poweroff` and `shutdown` work unchanged: they
mark `/etc/runit/stopit` (and `/etc/runit/reboot` for a reboot) executable and
send PID 1 `SIGCONT`, and schema-init answers that the way runit does. That is
also the path elogind takes, so the desktop's Restart and Shut Down work.
`shutdown` without `-h` or `-r` asks runit for single-user mode
(`runsvchdir single`), which schema-init does not have.

## Known

- **LVM, RAID, dm-crypt, ZFS and multi-device btrfs** are activated by runit's
  `03-filesystems.sh`, which schema-init does not run. A root on them is set up
  by the initramfs and boots; other filesystems on them are not activated.
- Void's elogind wrapper mounts a tmpfs on `/run/systemd` if there is none;
  `journal-sink` mounts it first so its socket is not hidden.
- Enabling elogind without rebooting leaves the seat without devices until the
  next boot (its udev rules arrived after coldplug). Same under runit.
