# schema-init on Artix Linux (beside dinit, s6 or OpenRC)

Boot Artix Linux with schema-init as PID 1, side by side with the init your
Artix install came with. Nothing about that boot changes: it stays the default
GRUB entry, and schema-init gets its own entries below it. Pick one at boot to
try it; pick the normal entry to go back.

The schema-init side is the same on every Artix edition. The daemons (udevd,
dbus, elogind, NetworkManager, LightDM, sshd) are the same binaries whichever
init's service scripts you installed, so one set of `.svc` files covers them.

Tested in UEFI VMs on **Artix (October 2026: kernel 7.2.10, udev 262)**, one
install per init (dinit 0.22.1, s6-rc 0.7.0 with s6-base 3.3.3, OpenRC 0.64.1),
each with Xfce 4.20 + LightDM autologin + elogind, NetworkManager and sshd. On
each: boots under schema-init to the Xfce desktop, Xfce's Restart and Shut Down
work, and the stock entry still boots with this installed. Not yet run on real
hardware.

## Compared with dinit, s6 and OpenRC

Measured in the same kind of VM (4 vCPU, virtio disk), one install per init,
each booted both ways 9 times alternating, medians of 8 (first discarded),
seconds from the kernel mounting the root filesystem. dinit and OpenRC were
measured on 2026-10-17; s6 again on 2026-10-18, after NetworkManager stopped
waiting for udev to settle:

| | dinit | schema-init | s6 | schema-init | OpenRC | schema-init |
|---|---|---|---|---|---|---|
| sshd started | 1.93 s | **0.24 s** | **0.10 s** | 0.20 s | 4.98 s | **0.24 s** |
| NetworkManager started | 1.92 s | **0.65 s** | **0.08 s** | 0.23 s | 3.83 s | **0.63 s** |
| desktop drawn (xfdesktop) | 4.21 s | **2.89 s** | 2.75 s | **2.48 s** | 7.04 s | **2.79 s** |
| RAM used after boot | **507 MB** | 512 MB | 518 MB | **510 MB** | 513 MB | **512 MB** |
| processes | **154** | 156 | 192 | **156** | 164 | **154** |

s6 is the closest: it starts sshd and NetworkManager about 0.1 s sooner;
schema-init draws the desktop 0.27 s sooner. Stock s6 and OpenRC on Artix can
also lose udev's coldplug events at boot and come up with no network and no
desktop. Method, raw data and the details: [Compared](../../docs/compared.md#measured).

## Install

```sh
sudo pacman -S --needed base-devel git pkgconf acl dbus zstd python
git clone https://github.com/ajax80/schema-init.git
cd schema-init
make
sudo distros/artix/install.sh
```

Then reboot and choose **schema-init: Artix Linux, with Linux linux** in the
GRUB menu.

Check it from a terminal:

```sh
sudo readlink /proc/1/exe     # /usr/bin/schema-init
sudo schema-ctl status        # every service FUNDAMENTAL or PERFECT
```

To remove the boot entries: `sudo distros/artix/install.sh --remove`.

## What install.sh does

- `make install`: binaries to `/usr/bin` (`schema-init`, `schema-ctl`,
  `schema-journal-sink`, ...).
- Copies the service files in `services/` to `/etc/schema-init/services/`,
  keeping any you have edited (it prints which), and the boot helpers in
  `scripts/` to `/usr/local/libexec/schema-init/`.
- Installs `/etc/grub.d/11_schema-init` and runs `grub-mkconfig`. That script
  runs GRUB's own `10_linux` with `init=/usr/bin/schema-init` added, so new
  kernels get a schema-init entry automatically.

## Services

| Service | Runs |
|---|---|
| `udevd`, `udev-trigger`, `udev-coldplug` | Artix's `udevd`, ready when `/run/udev/control` exists; then `udevadm control --ping`, retried up to 30 times a second apart (so udevd is listening for device events) and `udevadm trigger`; then `settle` |
| `network-manager` | starts once the coldplug events are queued (`udev-trigger`) and `sysctl` has run, not after they settle; NetworkManager picks up each device as udev finishes it |
| `mount-fstab` | fsck and mount `/etc/fstab`, swap; waits for each device itself, and for every member of a multi-device btrfs (`btrfs device scan`), so it starts with `udevd`, not after coldplug |
| `artix-boot` | hostname, loopback, `modules-load.d`, `hwclock --systz`, kmod static nodes, `etmpfiles`, utmp/wtmp/btmp, machine-id, sshd host keys |
| `sysctl` | `sysctl --system`, once the coldplug events are queued, before NetworkManager |
| `sysusers` | `esysusers`, off the boot chain (pacman's hook already runs it when a package adds a user) |
| `journal-sink` | owns `/dev/log`; logs to `/var/log/schema-init/journal.log` |
| `dbus`, `elogind`, `network-manager`, `lightdm`, `sshd`, `getty-tty1`…`6` | the daemons themselves, in the foreground |

PID 1 itself mounts `/proc`, `/sys`, `/dev`, `/run` and cgroup2, and remounts
`/` read-write.

Anything else you enabled for your Artix init (cronie, cups, bluetoothd, ...)
needs its own `.svc` in `/etc/schema-init/services/`; there is no importer for
dinit, s6 or OpenRC service definitions yet. See
[svc-reference](../../docs/svc-reference.md).

## Shutdown

On Artix, `halt`, `poweroff`, `reboot` and `shutdown` belong to the installed
init and talk to it, and elogind runs them by path, so the desktop's Restart
and Shut Down go through them. Under schema-init only, `artix-boot`
bind-mounts a small wrapper over each one that is a real file (on dinit,
`shutdown`; the others are symlinks to it) and the wrapper asks schema-init to
reboot or power off. Nothing on disk changes, so the stock boot and package
updates are unaffected. `shutdown` with a delay, `-c` or `-k` is refused.

## Known

- **No random seed.** Artix's inits each carry their own seedrng helper;
  schema-init does not save or credit a seed and leaves the RNG to the kernel.
- **LVM, RAID, dm-crypt and ZFS** set up by the initramfs boot; other
  filesystems on them are not activated.
- `/etc/rc.local` and the inits' own shutdown hooks are not run.
