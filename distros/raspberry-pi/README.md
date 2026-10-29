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

## Services first

The package ships example units only (`/usr/share/schema-init/*.svc.example`). Put your real service files in `/etc/schema-init/services/` before enabling. `schema-pi enable` refuses without `sshd.svc` there, so a failed boot can still be reached. See [Pi Zero W](../raspberry-pi-zero-w/README.md) for a working Wi-Fi chain (udev, wpa_supplicant, dhcpcd, sshd).

## Enable

```sh
sudo schema-pi enable
sudo reboot
```

`enable` and `disable` first mount `/boot/firmware` if `/etc/fstab` lists it and it is not mounted (schema-init does not automount fstab), and stop if that fails. `enable` refuses unless `/boot/firmware/cmdline.txt` exists, `/lib/systemd/systemd` exists, `/var/lib` is not `noexec`, and `sshd.svc` is present. Then it:

- backs up `cmdline.txt` to `cmdline.txt.schema-bak` (first run only)
- installs `pi-bootok.svc`, `pi-deadline.svc` and `pi-boot-firmware.svc` into `/etc/schema-init/services/` if missing (`pi-boot-firmware` mounts `/boot/firmware` at boot, so `cmdline.txt` and firmware updates land on the real partition)
- seeds `/var/lib/schema-init/known-good/` if it does not exist
- on the first line of `cmdline.txt`: replaces any `init=` with `init=/usr/sbin/schema-init-pi`, and appends `panic=10` if the line has no `panic=`

These three `pi-*.svc` files are copied once. Later package updates ship new `/usr/share/schema-init/pi-*.svc.example` files but never overwrite your copies. To pick up a changed example, copy it over by hand and reboot:

```sh
sudo cp /usr/share/schema-init/pi-bootok.svc.example /etc/schema-init/services/pi-bootok.svc
```

(the same for `pi-deadline` and `pi-boot-firmware`). Compare first with `diff` if you edited them.

It takes effect on the next boot. `schema-init-pi` is a small static program, so a broken `/bin/sh` or library after an upgrade cannot kill it.

## Boot fallback

"Boot OK" means `sshd` is up and every service is FUNDAMENTAL or PERFECT (none failed, excised or given up). Services still starting count as not ready, so boot OK waits for them. Skipped services and `pi-bootok` / `pi-deadline` themselves are ignored. A network address is not required: a router outage is not a schema failure and costs no reboots. A failed `wpa-supplicant` or `dhcpcd` still counts.

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

Prints `enabled`, `pid1`, `attempts`, `last_target`, `last_fallback` (last line of `fallback.log`) and `known_good` (version of the known-good engine).

## Disable

```sh
sudo schema-pi disable
sudo reboot
```

Strips `init=` from `cmdline.txt`; systemd boots next time. `apt remove schema-init` runs this for you (an upgrade does not). `panic=10` stays in place: rebooting on a kernel panic is harmless under systemd. Packages and known-good stay on disk.

## Recovery by hand

If the Pi will not come back: power off, put the SD card in another computer, mount the boot partition, and delete the `init=/usr/sbin/schema-init-pi` word from the first line of `cmdline.txt`. Keep it one line. Put the card back; the Pi boots systemd.

`cmdline.txt.schema-bak` is the line from the first `enable`. It may hold an older `init=` (for example a hand-installed wrapper) and may be stale, so edit the current line rather than copying the backup over it.
