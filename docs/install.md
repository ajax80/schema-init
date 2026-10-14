# Installing schema-init

## Quickstart

Replacing PID 1 sounds scary. It isn't, if you do it in the right order: your systemd boot entries stay in the GRUB menu, and you can back out with one reboot at every step. Pick the row that matches what you want:

| You want to… | Do this | What it touches |
|---|---|---|
| Try it without risking a disk | [Build, test, boot it in a VM](#build-from-source-any-distro) — or boot the installer ISO in a VM | Nothing on your machine |
| Put it on a spare machine | [Install from the ISO](#fresh-install--the-installer-iso) | The disk you pick in the installer |
| Convert your Fedora KDE install | [COPR + `schema-migrate`](#convert-fedora-kde-in-place) | Adds a non-default boot entry; one command undoes it |
| Run it on another distro | [`setup.sh` from source](#build-from-source-any-distro) | Adds a non-default boot entry |

### Fresh install — the installer ISO

Grab the prebuilt Fedora 44 installer (`schema-netinst44-installer-*.iso`) from the [latest release](https://github.com/ajax80/schema-init/releases/latest) and write it to a USB stick (or point a VM at it):

```sh
sudo dd if=schema-netinst44-installer-<version>.iso of=/dev/sdX bs=4M status=progress oflag=direct && sync
```

It is Fedora's normal installer: you pick the disk, user and timezone on the usual screens, and nothing is decided for you. It is a netinst image, so the machine needs a network connection during install (it pulls the KDE package set). The result is a full KDE desktop running **schema-init as PID 1**. Verified on real hardware — a clean install on a Dell i3 laptop boots straight to Plasma with no hand fixes.

The install is package-managed: schema-init, -daemons, -session and -migrate are RPMs and the `ajax80/schema-init` COPR repo is enabled, so `sudo dnf upgrade` brings later builds and PID 1 re-execs onto them in place. -migrate also brings the flip tools and health checks (in `/usr/libexec/schema-init`), the `systemctl` stand-in, and the import of any systemd unit a later package installs.

The stock systemd boot entries stay in the GRUB menu as a fallback. SELinux is off while schema-init is PID 1, so files written then carry no labels, and the imported `selinux-autorelabel-mark` unit flags the disk for relabelling. The first boot of a systemd entry after running schema-init therefore relabels the whole filesystem and reboots once. That takes a few minutes and is expected; it is what makes the fallback safe to use.

After the first login a wizard offers the *optional* **guided udev cutover** — that step retires `systemd-udevd` and hands `/dev` to schema-udev. schema-udev names network interfaces the way systemd does (`enp3s0`, `wlo1`, from the same `.link` files), so Wi-Fi and wired profiles keep working across the switch. Once that's confirmed healthy, it offers a second optional step on its own reboot: switching both the system and session bus to **schema-dbus**, with the same automatic rollback if the bus doesn't come up.

> ⚠️ **What the udev flip does:** it kills `systemd-udevd` and makes **schema-udev** authoritative over device management. This is the whole point — watching your init own `/dev` — but it *is* a real change to how the box handles hardware. It's optional and guided; skip it and you still get schema-init as PID 1 with stock udev underneath.

After the restart, just log back into your desktop — it finishes the switch on its own; you don't have to click anything. If you arm the switch but then never return to the desktop, a headless seatbelt safely undoes it after the next boot and puts you back on stock udev — no damage, nothing to clean up. (One consequence of that safety net: it's a *desktop* on-ramp — a machine you run headless won't keep the flip, because the confirmation comes from the graphical session coming up.)

<details>
<summary>Installed from the v0.4.1 ISO or earlier? Convert it to packages once.</summary>

Those installs have the files but not the packages. Convert once, then reboot (the edited service files hold off reloads and re-execs until then):

```sh
sudo dnf copr enable ajax80/schema-init
sudo dnf install schema-init schema-init-daemons schema-init-session schema-init-migrate
sudo schema-ctl reexec && sudo rm -f /usr/local/bin/schema-ctl
sudo sed -i -E 's#^exec=/usr/local/lib/schema/(schema-dbus-run\.sh|schema-udev-flip-healthcheck\.sh|schema-dbus-flip-healthcheck\.sh)$#exec=/usr/libexec/schema-init/\1#' /etc/schema-init/services/*.svc
sudo sed -i 's#/usr/local/lib/schema/schema-flip-apply#/usr/libexec/schema-init/schema-flip-apply#' /etc/sudoers.d/schema-flip
```

</details>

### Convert Fedora KDE in place

`schema-migrate` converts your running Fedora KDE install without a compiler. It reads your system, installs schema-init, ports the KDE session seam (seatd, `schema-logind`, autologin, PipeWire, polkit), bridges udev so the network and `/dev` come up, imports the units systemd had enabled, writes a **non-default** `(schema-init)` GRUB entry (your normal Fedora stays the default), makes the boot menu visible so you can pick it by hand, and heals the session gaps on first boot with `schema-doctor`.

```sh
sudo dnf copr enable ajax80/schema-init
sudo dnf install schema-init-migrate            # the migrator + schema-udev, prebuilt
sudo schema-migrate --discover                  # reads the system, changes nothing
sudo schema-migrate --deploy --prebuilt         # uses the packaged binaries
```

Reboot, pick the entry ending **(schema-init)** from the boot menu. To go back: boot your normal Fedora entry and run `sudo schema-migrate --uninstall` — it reverses exactly what it wrote (manifest-tracked) and leaves pre-existing packages alone.

Proven end-to-end in a Fedora-KDE VM: deploy → schema-init as PID 1 with a full Plasma desktop → `--uninstall` → back on systemd. On real hardware, the deploy (through the wizard below) has converted a stock Fedora 44 KDE laptop; `--uninstall` has only been run in the VM. v1 is Fedora KDE only; on anything else use `setup.sh` below.

Prefer a guided GUI? Install the wizard and start it:

```sh
sudo dnf copr enable ajax80/schema-init
sudo dnf install schema-init-wizard
schema-wizard                                   # or "Set up schema" in the app menu
```

`schema-init-wizard` adds a guided GUI around the same reversible flow, with a two-reboot safety ladder. It's been VM-tested end to end (deploy, udev flip, dbus flip, and forced rollbacks of both), and run on real hardware (2026-10-09): a stock Fedora 44 KDE laptop, `dnf install` from the COPR, all three stages healthy, no hand fixes ([numbers below](benchmarks.md#fedora-44-kde-converted-in-place-stock-vs-migrated)). The forced rollbacks have only been exercised in the VM. The CLI above does the same thing and backs out the same way.

Without the COPR, run the migrator from a clone; without `--prebuilt` it builds from source and pulls gcc/make via dnf the first time:

```sh
git clone https://github.com/ajax80/schema-init && cd schema-init
sudo python3 distros/fedora-installer/migrate/schema-migrate.py --deploy
```

**COPR channels.** `ajax80/schema-init` only gets tagged releases. Every commit to master also builds into `ajax80/schema-init-dev`; that repo is untested and meant for the author's own machines, so don't enable it on a box you depend on. The COPR builds `schema-init` (the init), `schema-init-daemons`, `schema-init-session`, `schema-init-migrate` (the migrator + `schema-udev`) and `schema-init-wizard` (the GUI).

### Build from source (any distro)

Four steps, safest first. Each one is worth doing before the next.

**Requirements:**

- **Build + test:** `gcc`, `make`, `pkg-config`, `libacl` headers (`libacl1-dev` on Debian/Ubuntu, `libacl-devel` on Fedora — the udev `uaccess` tests link `-lacl`), and `dbus-1` headers (`libdbus-1-dev` on Debian/Ubuntu, `dbus-devel` on Fedora — the default `make` target builds `schema-dbus` and `make test` compiles the sdbus tests against them). On Fedora the static link also needs `glibc-static` (Debian bundles `libc.a` in `libc6-dev`). Nothing else. The init itself is a single static binary with no runtime dependencies.
- **`schema-logind`:** additionally `python3-dbus` + `python3-gobject`.
- **Build a bootable ISO:** additionally **Docker** (or podman) — the ISO is built from a `debian:bookworm` container — plus `squashfs-tools` (`mksquashfs`), and network access to pull the base image. To boot that ISO in a window: `qemu-system-x86_64`, `xorriso`, `socat`.

#### 1. Build and test it (zero risk)

```sh
git clone https://github.com/ajax80/schema-init && cd schema-init
make            # build the static binary
make test       # ~130 test programs (C + Python): state machine, cgroups, udev, D-Bus, migrate, …
```

No root, no Docker, no VM — this just proves the code compiles clean and passes its test suite on your machine.

#### 2. Watch it boot in a VM (zero risk)

```sh
sudo scripts/make-iso.sh              # build a bootable schema-init ISO (needs Docker + squashfs-tools)
scripts/vmtest-gui.sh boot ~/schema-init.iso   # boot that ISO in QEMU (needs qemu + xorriso + socat)
```

`make-iso.sh` builds a full Debian + desktop live image with schema-init as PID 1 — it pulls a `debian:bookworm` container, so **Docker must be installed and running** and the first build downloads a few hundred MB. `vmtest-gui.sh` then boots the ISO you just built (it takes the ISO path as an argument — build it first). Nothing here touches your real bootloader or `/dev`.

#### 3. Install alongside systemd (reversible)

```sh
sudo ./setup.sh
```

The installer **does not replace systemd.** It compiles, installs the binaries, and:

- installs a `mount-fstab` service that mounts your disks, swap, and bind mounts from `/etc/fstab`, read fresh at every boot (`schema-mount-fstab`: parents first, waits for each device up to `x-systemd.device-timeout=` — 90 s, or 10 s with `nofail` — and a `nofail` entry never fails the boot; `noauto` and `_netdev` entries are skipped). `scripts/gen-mounts.sh` writes the service plus a `mount-fstab.sh` snapshot to check;
- optionally **imports your enabled systemd services** as `.svc` stubs so the box comes up running what it ran before (`scripts/gen-services.sh`);
- writes a **separate `schema-init (fallback)` GRUB entry** and leaves stock systemd as the default.

Reboot, pick **schema-init (fallback)** from the boot menu, and try it. If anything is wrong, reboot and choose your normal systemd entry — you're back, untouched. Iterate on your service files, boot the schema-init entry again. In this step `systemd-udevd` still runs; schema-init does not retire anything.

> Run `scripts/gen-mounts.sh` (preview) before rebooting and read the `mount-fstab.sh` snapshot — confirm every mount is right. It shows what your `/etc/fstab` decides; the boot reads the fstab itself, so fix any mistake there.

#### 4. Make it the default (once you trust it)

When the schema-init entry has booted cleanly a few times, make it default (set `GRUB_DEFAULT` / your distro's boot-entry default to it). Only then, if you want the full reclamation, opt into the [authoritative udev cutover](components.md#authoritative-mode-the-udevd-cutover) — a deliberate, checksum-backed, reversible flip that retires `systemd-udevd`. It is the advanced path; validate it in `schema-vmtest` LIVE mode first.

**A one-reboot undo (btrfs roots).** On a btrfs root, `make safe-install` snapshots `/` and `/home` into a writable sibling subvol with its own boot entry *before* installing — so a deploy that breaks the desktop is a 30-second rollback (pick the snapshot entry in GRUB, reboot) rather than a live-debugging session. No initramfs overlay needed. An optional boot-success guard (`schema-bootok` plus a `grub.d` hook) ties "the boot succeeded" to *the desktop actually came up* — not just PID 1 finishing — and auto-selects the last known-good snapshot after a failed boot. Setup and deploy flow: [docs/boot-success-guard.md](boot-success-guard.md).

**Porting to a machine that isn't yours yet** (e.g. setting it up for someone else): on that machine, while it's still on systemd, run `./setup.sh --generate-profile <name>` (no root, installs nothing) — it captures the machine's mounts and enabled services into a reusable `distros/<name>/` profile. Commit it, then `sudo ./setup.sh --profile <name>` brings the box up with its own disks and services. See [Porting to a new distro](porting.md#porting-to-a-new-distro) and the `distros/` profiles.

---

## Building

```sh
make
```

Produces a statically linked binary. One caveat: resolving `user=` and group names goes through glibc's NSS, which loads the host's `libnss_*` libraries at runtime, so build it on (or for) the distro you run it on. Tested on:

- Debian Bookworm, kernel 6.1, x86_64 — headless and Cinnamon desktop
- Fedora 44, kernel 7.0, x86_64 — full KDE Plasma desktop, btrfs subvolume boot

**Cross-compile for aarch64 (ARM — Ungulate Leg target):**

```sh
make aarch64
```

Requires `aarch64-linux-gnu-gcc`. On Fedora: `sudo dnf install gcc-aarch64-linux-gnu`. Produces static `schema-init-static`, `schema-ctl`, and `schema-subreaper` binaries. Override sysroot with `SYSROOT=/path/to/sysroot make aarch64`.

**ARM bare-metal (Pi Zero W, armv6l):**

Fedora's `arm-linux-gnu-gcc` cross-compiler does not ship an arm sysroot. Compile natively on the Pi:

```sh
sudo apt install git gcc make
git clone https://github.com/ajax80/schema-init
cd schema-init && make
```

The `armhf` Makefile target exists for environments that have a full arm sysroot available.

**schema-desktop (optional SDL2 monitor):**

```sh
make desktop
sudo cp desktop/schema-desktop /usr/local/bin/schema-desktop
```

Requires `SDL2` and `SDL2_ttf`. On Fedora: `sudo dnf install SDL2-devel SDL2_ttf-devel`. Reads live service state from PID 1's shared memory segment — run it from the desktop after login, or drop `distros/*/config/autostart/schema-desktop.desktop` into `~/.config/autostart/` to launch it automatically.

```sh
# install as PID 1 — symlink approach (distro-compatible)
cp schema-init /sbin/schema-init
ln -sf /sbin/schema-init /sbin/init

# or pass to kernel directly via GRUB
linux /boot/vmlinuz root=LABEL=my-root init=/sbin/schema-init
```

### GRUB setup

**Option A — symlink** (`/sbin/init` → `/sbin/schema-init`): works with any distro GRUB config, no kernel cmdline change needed. Replace your distro's init binary or point the symlink.

**Option B — explicit init= in GRUB**: add `init=/sbin/schema-init` to the kernel line in `/etc/default/grub`, then `grub-mkconfig -o /boot/grub/grub.cfg` (Debian/Ubuntu) or `grub2-mkconfig -o /boot/grub2/grub.cfg` (Fedora).

**Option C — custom GRUB menu entry**: create a separate entry that leaves the distro default untouched:

```
# /boot/grub/custom.cfg  (included automatically by grub.cfg)
menuentry 'schema-init' {
    search --no-floppy --label --set=root schema-root
    linux   /boot/vmlinuz-$(uname -r) root=LABEL=schema-root rw quiet init=/sbin/schema-init
    initrd  /boot/initramfs-$(uname -r).img
}
```

Option C is the safest for dual-boot or first-time installs — it leaves the existing systemd entry intact as a fallback.

**No initramfs? The root spec must be kernel-native.** schema-init happily boots with no initramfs (the kernel hands straight to PID 1), but then *nothing resolves a filesystem-level root spec for you* — `root=UUID=…` and `root=LABEL=…` are resolved by dracut/udev from inside the initramfs, which no longer runs. A no-initramfs kernel can only resolve a **kernel-native** device: `root=PARTUUID=…` (GPT partition UUID, from `blkid -s PARTUUID -o value /dev/…`), `root=PARTLABEL=…`, or `root=/dev/…`. If you drop the `initrd` line above, you **must** also switch `root=` to one of these, or the kernel panics before PID 1 with `VFS: Unable to mount root fs on unknown-block(0,0)` — schema-init never gets to run. Keep `rootflags=subvol=root` (or your subvol) for a btrfs root. The fs-UUID and the PARTUUID are different values; don't paste one where the other belongs.

**Kernel cmdline words are safe.** The kernel hands PID 1 every boot-cmdline token it didn't consume (`rhgb`, `quiet`, `splash`, `plymouth.debug`, …), so leave your usual options in the kernel line — schema-init ignores them when it runs as PID 1. A services directory other than the default `/etc/schema-init/services` can only be set by hand-running the binary (`schema-init /path/to/services`), never via the kernel cmdline.

### Replacing a running init (without reboot)

PID 1 can swap itself onto a new binary in place. Services keep running with the same pids, restart counts, timers and readiness state, and the desktop session never notices:

```sh
sudo schema-ctl reexec            # re-exec into the binary PID 1 booted from (now the new one on disk)
sudo schema-ctl reexec /path/bin  # or into a specific absolute path
# ok: re-executed into 0.4.0-1.990…, 60 services adopted, 0 new
```

**With the RPM, you don't need to run it.** On an upgrade, the package re-executes PID 1 itself once the transaction finishes, so `dnf upgrade` is the whole update. It is skipped in a chroot or container (the installer, mock) and when PID 1 isn't schema-init. A refusal is printed and never fails the transaction.

How it stays safe:

- **Dry run first.** The new binary is started as a child with `--reexec-check` and must accept the state before PID 1 commits. A binary that crashes, is the wrong program, or can't read the state is refused, and the running PID 1 is untouched.
- **State is handed over, not re-derived.** The runtime half of every service is serialized as versioned text into a sealed memfd. The control, notify and watchdog fds are inherited. The new image skips boot-only work (mounts, module loading, `/tmp` lock cleanup, watchdog arming) and adopts the running children.
- **Fallback.** If the new image fails after the swap, it re-executes the old one (held open by fd) and the command replies `err: rolled back`.
- **Refused** when a `.svc` changed since boot (those changes need a reboot), while PID 1 is in its survival posture (a memory stall, or a critical service starved for CPU, within the last 5 s; the refusal names which), or for a relative path. `schema-ctl` exits 1 on any refusal.

Measured on blakbox (60 services, full Plasma session): 53 ms per re-exec, `schema-ctl status` byte-identical before and after.

One caveat: a PID 1 older than re-exec support (COPR before `0.4.0-1.983`, git before `v0.4.0-31`) has no `reexec` verb, so moving off it takes one last reboot.

If you install the binary by hand, it can't be overwritten while it runs (`text file busy`). Copy it next to the old one and `mv` it over, which replaces the directory entry atomically, then re-exec:

```sh
cp schema-init /usr/bin/schema-init.new
mv /usr/bin/schema-init.new /usr/bin/schema-init
sudo schema-ctl reexec
```

---

## Filesystem setup

schema-init does not parse `/etc/fstab`. On boot it mounts the pseudo-filesystems directly:

| Mount | Type | Notes |
|-------|------|-------|
| `/` | remount rw | Kernel mounts rootfs read-only for fsck; schema-init remounts it writable before anything else |
| `/proc` | proc | nosuid, nodev, noexec |
| `/sys` | sysfs | nosuid, nodev, noexec |
| `/dev` | devtmpfs | nosuid, strictatime |
| `/dev/pts` | devpts | nosuid, noexec, `gid=5,mode=620,ptmxmode=666` — without it there are no PTYs and every terminal emulator fails to start |
| `/dev/shm` | tmpfs | nosuid, nodev, mode=1777 — POSIX shared memory |
| `/run` | tmpfs | nosuid, nodev, mode=0755 |
| `/sys/fs/cgroup` | cgroup2 | nosuid, nodev, noexec, relatime |

After mounting `/dev`, schema-init creates four symlinks that devtmpfs does not provide and a userspace init is expected to make itself:

| Link | Target |
|------|--------|
| `/dev/fd` | `/proc/self/fd` |
| `/dev/stdin` | `/proc/self/fd/0` |
| `/dev/stdout` | `/proc/self/fd/1` |
| `/dev/stderr` | `/proc/self/fd/2` |

Without these, bash process substitution (`< <(...)`) and any `/dev/stdin`-style redirect fail — a gap that surfaces in ordinary shell scripts long before it surfaces anywhere in the init itself.

schema-init also creates `/run/log/schema-init/` at boot. Each service's stdout and stderr are redirected there automatically (see Logs).

If your system needs additional mounts (data partitions, network filesystems), run them as `oneshot` services before your other services depend on them.

> **Mount by `UUID=` or `LABEL=`, never `/dev/sdX`.** The kernel assigns `sda`/`sdb`/… in detection order, which can change between boots — so a oneshot that mounts `/dev/sdb1` may silently land on the wrong physical disk, swapping two data drives and pointing every absolute path at the wrong filesystem. `/etc/fstab` under systemd hid this by mounting by UUID for you; schema-init doesn't read fstab, so do it explicitly: `mount UUID=1b7d654f-… /mnt/data` (or `LABEL=`). The same applies to the root `LABEL=`/`PARTUUID=` on the kernel cmdline.
