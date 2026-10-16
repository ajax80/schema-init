# schema-init on Devuan

Boot Devuan 6 (Excalibur) with schema-init as PID 1, side by side with sysvinit.
Nothing about your sysvinit boot changes: it stays the default GRUB entry, and
schema-init gets its own entries below it. Pick one at boot to try it; pick the
normal entry to go back.

Tested on a fresh Devuan 6.1.1 netinstall (sysvinit, Xfce, SLiM, elogind, eudev,
NetworkManager) in a UEFI VM: boots to the Xfce desktop under schema-init, the
Xfce Restart/Shut Down buttons work, and the sysvinit entry still boots and
shuts down normally with this installed.

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
  keeping any you have edited (it prints which) and skipping any whose program
  isn't installed (LCOS, for example, has no cron).
- Copies the boot helpers in `scripts/` to `/usr/local/libexec/schema-init/`.
- Diverts `/usr/sbin/halt` and `/usr/sbin/shutdown` with `dpkg-divert` and puts a
  small wrapper in their place (see Shutdown below). sysvinit's originals stay
  as `halt.sysvinit` and `shutdown.sysvinit`.
- Installs `/etc/grub.d/11_schema-init` and runs `update-grub`. That script
  runs Debian's own `10_linux` with `init=/usr/bin/schema-init` added, so new
  kernels get a schema-init entry automatically.

## Services

| Service | Replaces (sysvinit) | Runs |
|---|---|---|
| `udevd`, `udev-coldplug` | `eudev` | `/sbin/udevd`, then `udevadm trigger` + `settle` |
| `mount-fstab` | `mountall.sh` | mounts `/etc/fstab` |
| `devuan-boot` | `bootmisc.sh`, `x11-common`, `networking` (lo), `kmod`, `hwclock.sh` | loopback up, `swapon -a`, `/etc/modules`, RTC if `LOCAL`, `/tmp/.X11-unix`, `/run/sshd`, `/run/utmp`, machine-id |
| `sysctl` | `procps` | `sysctl --system` (a key the kernel rejects is logged, not fatal) |
| `journal-sink` | `rsyslog` | owns `/dev/log`; logs to `/var/log/schema-init/journal.log` |
| `dbus` | `dbus` | `dbus-daemon --system --nofork` |
| `elogind` | `elogind` | `/usr/libexec/elogind` |
| `network-manager` | `network-manager` | `NetworkManager --no-daemon` |
| `slim` or `lightdm` | `slim` / `lightdm` | `slim -n` / `lightdm`; install.sh picks the one in `/etc/X11/default-display-manager` |
| `sshd`, `cron` | `ssh`, `cron` | `sshd -D`, `cron -f` |
| `chrony`, `bluetoothd` | `chrony`, `bluetooth` | `chronyd -n`, `bluetoothd -n` |
| `getty-tty1` … `getty-tty6` | `inittab` gettys | `getty 38400 ttyN` |
| `logrotate` | cron.daily logrotate | `logrotate` at 00:10 |

PID 1 itself mounts `/proc`, `/sys`, `/dev`, `/run`, remounts `/` read-write and
sets the hostname from `/etc/hostname`.

Not started under schema-init: rsyslog (journal-sink owns `/dev/log`), seatd
(elogind manages the seat), anacron, avahi, cups, saned, ModemManager. To add one, write a `.svc` in `/etc/schema-init/services/`;
`schema-ctl add <path>` loads it without a reboot.

Logs: `/var/log/schema-init/<service>.log`, boot and shutdown timing in
`rail.log` and `shutdown.log`.

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

- `tmpfs` lines in `/etc/fstab` (and `RAMTMP=yes`) are not mounted under
  schema-init; `/tmp` stays on disk.
- `halt` powers off. Under schema-init `shutdown` only takes `now` (no delayed
  shutdown); `-w` and `--help` go to sysvinit's tools.
- No fsck at boot under schema-init (sysvinit's `checkroot.sh`/`checkfs.sh` do
  that). Boot the sysvinit entry now and then, or `touch /forcefsck` before it.
- `last` shows sessions ending in "crash": schema-init does not write wtmp
  boot/shutdown records.
- SLiM and LightDM are supported. Another display manager needs its own `.svc`
  (run it in the foreground); install.sh says so when it finds none.
- LightDM's very first start on a fresh install can fail once while it creates
  `/var/lib/lightdm/data`; schema-init restarts it and it comes up.
- No GUI wizard on Devuan; `install.sh` is the whole path.

EXPERIMENTAL. Try it on a machine you can reinstall.
