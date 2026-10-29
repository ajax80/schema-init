# schema-init — Raspberry Pi

Runs schema-init as PID 1 on Raspberry Pi OS (Debian 13, trixie) from the signed apt repo, with a boot fallback so a bad boot does not need the SD card pulled.

Packages: `armhf` (ARMv6, built on Raspbian, so it runs on every Pi including the Zero and 1), `arm64`, `amd64`. On amd64 it is packages only: `schema-pi` and the boot guard are for Raspberry Pi.

Installing the package never changes PID 1. `schema-pi enable` is the opt-in.

## Install

Channels: `stable` is tagged releases, `dev` is every master commit.

Signing key fingerprint: `3ABC 17E5 9514 3A99 E5F2  4246 62CB 71B0 B40C 9EF8`

```sh
sudo curl -fsSLo /usr/share/keyrings/schema-init-archive-keyring.gpg \
    https://ajax80.github.io/schema-init-apt/schema-init-archive-keyring.gpg
gpg --show-keys /usr/share/keyrings/schema-init-archive-keyring.gpg   # check the fingerprint
```

`/etc/apt/sources.list.d/schema-init.sources` (use `Suites: stable` for releases):

```
Types: deb
URIs: https://ajax80.github.io/schema-init-apt
Suites: dev
Components: main
Signed-By: /usr/share/keyrings/schema-init-archive-keyring.gpg
```

```sh
sudo apt update
sudo apt install schema-init
```

## What enable sets up

`schema-pi enable` installs a small core service kit, imports everything systemd had enabled, and writes the boot-OK list. You do not write service files first. Kit files come from `/usr/share/schema-init/<name>.svc.example` and are copied into `/etc/schema-init/services/` only when absent, so your edits are never overwritten.

The core kit:

- `pi-early`: oneshot that brings `lo` up (127.0.0.1 and ::1), sets the hostname from `/etc/hostname` and runs `swapon -a`. It always exits 0.
- `udev`: `systemd-udevd`, after `pi-early`.
- `udev-trigger`: oneshot coldplug (`udevadm trigger`, then `settle` for up to 15 s), after `udev`. Best effort, never fails the boot.
- `dbus`: the system bus daemon, after `pi-early`; ready when `/run/dbus/system_bus_socket` exists. It must keep the name `dbus`, because imported services that need the bus depend on that name.
- `pi-mounts`: oneshot `mount -a -O no_netdev` after `udev-trigger`, so the rest of `/etc/fstab` (a USB disk, a bind mount) is mounted; schema-init has no fstab automount. Network mounts are left out. Best effort: a mount that fails is logged and the boot goes on, and it is not in the boot-OK list.
- `pi-timesync`: Pis have no clock battery. It waits for a default route, then sets the clock from the HTTP `Date` header of `http://1.1.1.1/` (then `http://8.8.8.8/`), so it needs no DNS. The header is not authenticated, so it never sets the clock back: a `Date` before the current clock or the package build date is logged and ignored. It retries until it succeeds, then resyncs every 6 hours, and never fails the boot.
- `journal-sink`: owns `/dev/log` and the journal socket (`schema-journal-sink`). Do not add an `rsyslog.svc`.
- `pi-boot-firmware`, `pi-bootok`, `pi-deadline`: mount `/boot/firmware`, confirm boot OK, and reboot if it never happens (see Boot fallback).

Then `enable` imports every service systemd had enabled (NetworkManager, sshd, pihole-FTL, ...). It prints `imported N, skipped M` and one `skip <unit>: <reason>` line per unit it did not import. A unit is skipped, for example, when its binary already runs as one of your own `.svc` files, or when its type cannot be translated. Look at the skip lines: anything you need there you must write by hand. `systemd-*` units are not imported (schema-init replaces them), except the network service when that is `systemd-networkd`; `enable` names the enabled ones it left out (`not imported (schema-init replaces): ...`).

The network service is the first enabled of NetworkManager, systemd-networkd, networking, dhcpcd, wpa_supplicant. `enable` refuses if none is enabled, or if no ssh service (`ssh` or `sshd`) exists, so a failed boot can still be reached. The network service gets `udev-trigger` and `dbus` as dependencies if it lacks them (written to a `pi-kit.conf` drop-in, never into your file). `enable` refuses if that would give it more than 8 `dep=` lines, the engine's limit. It runs the import once into a scratch directory first, so these refusals happen before anything is written.

### Boot-OK list

`/etc/schema-init/pi-boot-required` holds one service name per line; `#` comments and blank lines are ignored. Boot OK waits for exactly these to be FUNDAMENTAL, SETTLED or PERFECT. `enable` writes:

- `udev`, `udev-trigger`, `dbus`, `pi-early`
- ssh (`ssh` or `sshd`, whichever was imported) and the detected network service
- `pihole-FTL` when its `.svc` exists, and `unbound` only when both `pihole-FTL.svc` and `unbound.svc` exist

`pi-mounts`, `pi-timesync`, `journal-sink`, `pi-boot-firmware` and every other import are not in it: they may fail without costing a reboot. `pi-bootok` and `pi-deadline` are never counted even if listed.

You can add your own lines (a service you cannot boot without). Later `enable` runs keep your lines and comments and add any missing generated names. A line whose `.svc` is gone is dropped, with a message, because boot OK would wait on it forever and the deadline would fall back. If the file is missing (or lists no names), every service must be FUNDAMENTAL, SETTLED or PERFECT, as described under Boot fallback. A known-good boot reads the copy of the list saved in known-good.

### Remove a hand-written set before enable

If you wrote `.svc` files by hand for daemons that systemd's units also run (NetworkManager, sshd, ...), check how they start them. When your `.svc` runs the daemon's binary directly, the importer sees it, skips that unit, and `enable` uses your file. When it starts the daemon through a wrapper script, the importer cannot tell, imports the unit too, and you get two copies. Remove those first. `enable` warns on stderr, before importing, about every `.svc` that is neither a kit file nor imported by an earlier `enable`: `hand-written services found: <names>`. It only warns. A Pi enabled before this warning existed lists its earlier imports once, on the next enable.

Pi Zero W notes and gotchas: [Pi Zero W](../raspberry-pi-zero-w/README.md).

## Enable

```sh
sudo schema-pi enable
sudo reboot
```

`enable` and `disable` first mount `/boot/firmware` if `/etc/fstab` lists it and it is not mounted (schema-init does not automount fstab), and stop if that fails. `enable` refuses unless `/boot/firmware/cmdline.txt` exists, `/lib/systemd/systemd` exists, `/var/lib` is not `noexec`, a network service and an ssh service are enabled. Then it:

- backs up `cmdline.txt` to `cmdline.txt.schema-bak` (first run only)
- installs the kit and imports the enabled systemd services (above)
- seeds `/var/lib/schema-init/known-good/` if it does not exist
- on the first line of `cmdline.txt`: replaces any `init=` with `init=/usr/sbin/schema-init-pi`, and appends `panic=10` if the line has no `panic=`

Kit files are copied once. Later package updates ship new `/usr/share/schema-init/*.svc.example` files but never overwrite your copies. To pick up a changed example, copy it over by hand and reboot:

```sh
sudo cp /usr/share/schema-init/pi-bootok.svc.example /etc/schema-init/services/pi-bootok.svc
```

(the same for any other kit file). Compare first with `diff` if you edited them.

It takes effect on the next boot. `schema-init-pi` is a small static program, so a broken `/bin/sh` or library after an upgrade cannot kill it.

## Boot fallback

"Boot OK" means every service in `pi-boot-required` is FUNDAMENTAL, SETTLED or PERFECT (none failed, excised or given up). Services still starting count as not ready, so boot OK waits for them. Without the list, `sshd` must be up and every service must be FUNDAMENTAL, SETTLED or PERFECT; skipped services and `pi-bootok` / `pi-deadline` themselves are ignored. A network address is not required: a router outage is not a schema failure and costs no reboots. With the list, a failed network service still counts because it is listed.

- Each boot of schema-init increments a counter before exec. Boot OK resets it to 0.
- If boot OK has not happened 300 s after `pi-deadline` starts, `pi-deadline` reboots the Pi (`schema-ctl reboot`). It does nothing unless the kernel was booted with `init=/usr/sbin/schema-init-pi`.
- If a binary will not exec, the wrapper records the next target (known-good, then systemd) before trying it, so a broken current is never promoted. If the boot state cannot be saved at all (read-only or full `/var`), the wrapper boots systemd: without a counter the deadline would reboot forever.
- After two failed boots in a row, the third boot goes to the known-good set if it differs from the current one (engine binary or services). Otherwise it goes to systemd.
- A known-good or systemd boot lasts one boot only. `init=` stays in `cmdline.txt`. A successful known-good boot resets the counter but does not promote anything, so the next reboot tries the current set again. The cycle (two failed boots, one fallback boot) repeats until the current set is fixed or you run `schema-pi disable`. If known-good fails too, the boot after it is systemd.
- On boot OK the current set is promoted to known-good, only when that boot ran the current set.

State is in `/var/lib/schema-init/pi-boot` (`attempts=`, `last_target=`). Every fallback is appended to `/var/log/schema-init/fallback.log`; the boot log is `/var/log/schema-init/boot.log`. PID 1 also holds `/dev/watchdog` for true hangs, which needs the `bcm2835_wdt` driver loaded (check `ls /dev/watchdog`).

`apt upgrade` of `schema-init` while schema-init is PID 1 re-execs PID 1 onto the new binary in place (`schema-ctl reexec`); services keep running. If it refuses, the old binary keeps running until reboot.

## Status

```sh
sudo schema-pi status
```

Prints `enabled`, `pid1`, `attempts`, `last_target`, `last_fallback` (last line of `fallback.log`), `known_good` (version of the known-good engine) and, when something is not up, a `not up:` line (`name=STATE`, or `name=MISSING` for a listed service with no state), from the same rule boot OK uses. It names what blocks boot OK and also any uncounted service that is not up. No line means everything is up.

## Disable

```sh
sudo schema-pi disable
sudo reboot
```

Strips `init=` from `cmdline.txt`; systemd boots next time. `apt remove schema-init` runs this for you (an upgrade does not). `panic=10` stays in place: rebooting on a kernel panic is harmless under systemd. Packages and known-good stay on disk.

## Recovery by hand

If the Pi will not come back: power off, put the SD card in another computer, mount the boot partition, and delete the `init=/usr/sbin/schema-init-pi` word from the first line of `cmdline.txt`. Keep it one line. Put the card back; the Pi boots systemd.

`cmdline.txt.schema-bak` is the line from the first `enable`. It may hold an older `init=` (for example a hand-installed wrapper) and may be stale, so edit the current line rather than copying the backup over it.
