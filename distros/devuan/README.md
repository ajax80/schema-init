# schema-init on Devuan (and LCOS)

Boot Devuan 6 (Excalibur) with schema-init as PID 1, side by side with sysvinit.
Nothing about your sysvinit boot changes: it stays the default GRUB entry, and
schema-init gets its own entries below it. Pick one at boot to try it; pick the
normal entry to go back.

Tested in UEFI VMs on:

- **Devuan 6.1.1** netinstall (sysvinit, Xfce, SLiM, elogind, eudev,
  NetworkManager), and the same with LightDM.
- **LCOS 0.9.3** (Devuan Excalibur, LightDM, XLibre, chrony, kernel
  7.2.6-lunduke).

- **LCOS 0.9.3 on real hardware**: HP 15 laptop (Celeron N4000, 4 GB, HDD,
  Wi-Fi), dual-booted beside Fedora. Measured: desktop 1.9 s sooner and 9 MB
  less RAM than sysvinit ([numbers](../../docs/benchmarks.md#lcos-093-devuan-based-xfce-sysvinit-vs-schema-init)).

On each: boots to the desktop under schema-init, the Restart/Shut Down buttons
work, and the sysvinit entry still boots and shuts down normally with this
installed.

## Compared with sysvinit

What you get:

- **Supervision.** sysvinit starts a daemon and forgets it. If
  NetworkManager or elogind dies, it stays dead. schema-init restarts it and
  counts the restarts (`schema-ctl status`).
- **Start on ready, not on order.** A service starts once the ones it depends
  on are actually up (elogind waits for the D-Bus socket to exist, not for a
  script before it to return).
- **Ordered shutdown.** Services stop in reverse dependency order, then what
  is left is killed and filesystems go read-only. On the LCOS VM the services
  were down in 0.6 s.
- **A log per service** in `/var/log/schema-init/`, plus boot and shutdown
  timing.
- **Live control.** `schema-ctl status`, `start`, `stop`, and `add` to load a
  new service without a reboot. A new schema-init takes over PID 1 in place
  (`schema-ctl reexec`).
- **Timers without cron** (`logrotate` at 00:10).
- **A rescue boot and fsck at boot**, as with sysvinit (see
  [Rescue](#rescue)). A filesystem fsck cannot fix stops the boot only until
  you exit the rescue shell; it is then mounted if it checks clean, or left
  out, so a machine with a locked root account still boots.
- **Per-service sandboxing** (`no_new_privs`, `protect_system`,
  `private_tmp`, `protect_home`) is available; the Devuan services ship with
  it off.

What it costs:

- Services are translated, not native. Most Devuan packages ship a systemd
  unit beside their init script, and schema-import reads it; the rest run their
  init script once, as sysvinit does, without supervision. See
  [Other services](#other-services).
- No runlevels. The rescue boot is a shell before any service starts, not a
  runlevel with some of them running. No wtmp: see [Known](#known).
- sysvinit has decades of use behind it. This path was first tested in
  October 2026.

## Install

```sh
sudo apt install git build-essential pkg-config libacl1-dev libdbus-1-dev libzstd-dev
git clone https://github.com/ajax80/schema-init.git
cd schema-init
make
sudo distros/devuan/install.sh
```

Then reboot and choose **schema-init: Devuan GNU/Linux, with Linux …** in the
GRUB menu. GRUB waits 5 seconds by default; press an arrow key to stop the countdown.

Check it from a terminal:

```sh
sudo readlink /proc/1/exe     # /usr/bin/schema-init
sudo schema-ctl status        # every service FUNDAMENTAL or PERFECT
```

## What install.sh does

- `make install`: binaries to `/usr/bin` (`schema-init`, `schema-ctl`,
  `schema-journal-sink`, ...).
- Copies the service files in `services/` to `/etc/schema-init/services/`,
  keeping any you have edited (it prints which). Services for optional programs
  (cron, chrony, bluetoothd, sshd, ...) carry `condition=file_is_exec:`, so they
  are skipped at boot while the program isn't installed (LCOS has no cron) and
  start once it is.
- Copies the boot helpers in `scripts/` to `/usr/local/libexec/schema-init/`.
- Runs `schema-import --rc` for everything else sysvinit starts, and adds an
  apt hook that reruns it after every install (see
  [Other services](#other-services)).
- Diverts `/usr/sbin/halt` and `/usr/sbin/shutdown` with `dpkg-divert` and puts a
  small wrapper in their place (see Shutdown below). sysvinit's originals stay
  as `halt.sysvinit` and `shutdown.sysvinit`.
- Installs `/etc/grub.d/11_schema-init` and runs `update-grub`. That script
  runs Debian's own `10_linux` with `init=/usr/bin/schema-init` added, so new
  kernels get a schema-init entry automatically.

## Services

| Service | Replaces (sysvinit) | Runs |
|---|---|---|
| `udevd`, `udev-trigger`, `udev-coldplug` | `eudev` | `/sbin/udevd`, then `udevadm trigger`, then `settle` |
| `mount-fstab` | `mountall.sh` | mounts `/etc/fstab`; waits for each device itself, so it starts with `udevd`, not after coldplug |
| `devuan-boot` | `bootmisc.sh`, `x11-common`, `networking` (lo), `kmod`, `hwclock.sh` | loopback up, `swapon -a`, `/etc/modules`, RTC if `LOCAL`, `/tmp/.X11-unix`, `/run/sshd`, `/run/utmp`, machine-id |
| `sysctl` | `procps` | `sysctl --system` (a key the kernel rejects is logged, not fatal) |
| `journal-sink` | `rsyslog` | owns `/dev/log`; logs to `/var/log/schema-init/journal.log` |
| `dbus` | `dbus` | `dbus-daemon --system --nofork`, or schema-dbus (below) |
| `elogind` | `elogind` | `/usr/libexec/elogind` |
| `network-manager` | `network-manager` | `NetworkManager --no-daemon`, once the coldplug events are queued (`udev-trigger`), not after they settle |
| `slim` or `lightdm` | `slim` / `lightdm` | `slim -n` / `lightdm`; install.sh picks the one in `/etc/X11/default-display-manager` |
| `sshd`, `cron` | `ssh`, `cron` | `sshd -D`, `cron -f` |
| `chrony`, `bluetoothd` | `chrony`, `bluetooth` | `chronyd -n`, `bluetoothd -n` |
| `getty-tty1` … `getty-tty6` | `inittab` gettys | `getty 38400 ttyN` |
| `logrotate` | cron.daily logrotate | `logrotate` at 00:10 |

PID 1 itself mounts `/proc`, `/sys`, `/dev`, `/run`, remounts `/` read-write and
sets the hostname from `/etc/hostname`.

### Other services

Everything else in `/etc/rc2.d` (what sysvinit starts in runlevel 2) gets a
`.svc` from `schema-import --rc`:

- If the package ships a systemd unit that boot starts (`WantedBy=`), the
  `.svc` comes from the unit: supervised, with its socket, readiness, user,
  environment file and sandboxing carried over (cups, avahi-daemon, rsync,
  haveged, ...).
- Otherwise (no unit, a masked one, one only udev or D-Bus starts, or one
  schema-import cannot translate) it runs `/etc/init.d/<name> start` once, as
  sysvinit does (rc.local, sudo, rmnologin, saned, spice-vdagent, ...). These
  are not supervised and get no `stop` at shutdown: whatever they started is
  killed with the rest after the services stop.

The apt hook (`/etc/apt/apt.conf.d/80schema-init`) does the same after every
`apt install`; under schema-init the new service starts right away.
`/etc/schema-init/services/<name>.svc.skipped` says why a service was not
translated (e.g. open-vm-tools outside VMware). A `.svc` you write or edit is
never overwritten.

Left out on purpose ([`sysv-covered`](sysv-covered)): scripts a service above
already runs, plus rsyslog (journal-sink owns `/dev/log`), seatd (elogind
manages the seat) and bootlogd.

### schema-dbus

schema-dbus is schema-init's own system bus broker. It is off by default. To
use it in place of `dbus-daemon` on the next schema-init boot:

```sh
sudo touch /etc/schema-init/dbus-broker
```

At boot the launcher turns `/usr/share/dbus-1/system.conf` (and the files it
includes) into schema-dbus's policy, then starts schema-dbus. If the file is
missing or the policy cannot be read, it starts `dbus-daemon` instead.
schema-dbus starts D-Bus-activated services (UDisks2, UPower, polkit, …)
itself. `sudo rm /etc/schema-init/dbus-broker` goes back to `dbus-daemon`.
sysvinit boots always use `dbus-daemon`.

On the HP laptop (LCOS, HDD) schema-dbus drew the desktop in 25.8 s against
26.2 s for `dbus-daemon` (median of 3 boots each), with the same RAM: on par,
within boot-to-boot noise ([raw](../../tests/livetest/dbox-bench-20261016-schema-dbus.txt)).

Logs: `/var/log/schema-init/<service>.log`, boot and shutdown timing in
`rail.log` and `shutdown.log`.

## Rescue

The **schema-init: … (recovery mode)** GRUB entry (or `schema.rescue`,
`single` or `1` on the kernel command line) asks for root's password and gives
a shell before any service starts. `/proc`, `/sys`, `/dev` and `/run` are
mounted and `/` is writable; nothing from `/etc/fstab` is (`mount -a`). Exit
the shell to boot normally. With the root account locked, sulogin gives no
shell and the boot goes on.

At boot, `mount-fstab` runs `fsck -a` on each `/etc/fstab` filesystem with a
pass number (the sixth field) above 0 before mounting it. `/` is checked by the
initramfs, as under sysvinit. A FAT filesystem (the EFI partition) is not scanned
when its boot sector and FAT both say it was unmounted cleanly (Linux and
Windows each mark it dirty while mounted): on the HP laptop's HDD that scan
cost about 0.6 s of boot. Damage that a clean unmount does not record, such as
a bad sector, is then found only by a forced check. `forcefsck` on the kernel command line or
`touch /forcefsck` forces the check; `fastboot` or `/fastboot` skips it. Both files are removed after one boot. When fsck cannot fix
a filesystem, a `nofail` one is left unmounted; any other gets the same
root-password shell, and is mounted after it if fsck then passes. Results are in
`/var/log/schema-init/mount-fstab.log`.

## Shutdown

elogind (the Xfce Shut Down / Restart buttons) and `sudo reboot` run
`/usr/sbin/reboot`, `poweroff`, `halt` or `shutdown`. sysvinit's versions talk to
sysvinit over `/run/initctl`, which schema-init does not serve. The wrapper
checks which init is running: under schema-init it calls `schema-ctl reboot` or
`schema-ctl poweroff`; under sysvinit it runs sysvinit's original with the same
arguments. `shutdown -c` and `-k` are refused under schema-init.

## Remove

```sh
sudo distros/devuan/install.sh --remove
```

Removes the GRUB entries, the diversions, the boot helpers and schema-init's
polkit D-Bus activation file (Devuan's own takes over). `/etc/schema-init`
and the binaries in `/usr/bin` stay; delete them by hand if you want them gone.

## Known

- A `.svc` from `schema-import` stays after `update-rc.d <name> disable` or
  `apt remove`; delete it by hand. Timers (`man-db.timer`, ...) are not
  imported. `rc.local` is not held back until the end of boot as under
  sysvinit.

- `tmpfs` lines in `/etc/fstab` (and `RAMTMP=yes`) are not mounted under
  schema-init; `/tmp` stays on disk.
- `halt` powers off. Under schema-init `shutdown` only takes `now` (no delayed
  shutdown); `-w` and `--help` go to sysvinit's tools.
- `/forcefsck` forces the check of the other filesystems, not of `/`: the
  initramfs checks `/` before schema-init starts. Use `forcefsck` on the kernel
  command line for both.
- `last` shows sessions ending in "crash": schema-init does not write wtmp
  boot/shutdown records.
- SLiM and LightDM are supported. Another display manager needs its own `.svc`
  (run it in the foreground); install.sh says so when it finds none.
- LightDM autologin: on the HP laptop, 1 schema-init boot in 14 came up at the
  LightDM login screen instead of the desktop (the session exited with "Authorization
  required" after a 10 s stall); 0 in 13 sysvinit boots. Logging in by hand works.
  The cause is not found yet; LCOS works around a similar X cookie race in
  `lcos-display-fix`.
- LightDM's very first start on a fresh install can fail once while it creates
  `/var/lib/lightdm/data`; schema-init restarts it and it comes up.
- No GUI wizard on Devuan; `install.sh` is the whole path.

EXPERIMENTAL. Try it on a machine you can reinstall.
