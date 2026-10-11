# Porting and distributions

## Porting to a new distro

Starting from scratch on a distro not in `distros/`:

**1. Build the binary on the target (or cross-compile):**
```sh
git clone https://github.com/ajax80/schema-init
cd schema-init && make
```

**2. Install:**
```sh
sudo cp schema-init /sbin/schema-init
sudo cp schema-ctl  /usr/local/bin/schema-ctl
sudo mkdir -p /etc/schema-init/services
```

**3. Write service files.** Start minimal — just enough to reach a console:

```ini
# /etc/schema-init/services/udevd.svc
name=udevd
exec=/usr/lib/systemd/udevd
args=--daemon
needs_root=1
stable_secs=3

# /etc/schema-init/services/dbus.svc
name=dbus
exec=/usr/bin/dbus-daemon
args=--system
args=--nofork
needs_root=1
stable_secs=2
ready_path=/run/dbus/system_bus_socket
```

The udevd path varies by distro: `/usr/lib/systemd/udevd` (Fedora/Debian), `/lib/udev/udevd` (older Debian), `/usr/bin/udevd` (Arch).

**4. Configure GRUB** (see Building → GRUB setup above). Boot with a fallback entry pointing at systemd so you can recover.

**5. Boot and check:**
```sh
sudo schema-ctl list       # all services should reach FUNDAMENTAL
sudo schema-ctl timing     # see where time goes
tail /run/log/schema-init/udevd.log   # if something is EXCISED, check its log
```

**6. Add services incrementally.** Bring up network, then login manager, then display manager. Add `dep=` links to enforce order. Add `ready_path=` for anything with a socket or pidfile.

**7. Handle D-Bus hangs.** Open your desktop's settings panel immediately after first login. If it hangs >5s, run `dbus-monitor --system` and identify the missing interface. Add a stub to schema-logind or mask the activation file.

**Common issues by distro:**

| Issue | Cause | Fix |
|-------|-------|-----|
| udevd not populating /dev/input | udev not settled before display manager | `dep=udev` in display manager svc; `udevadm settle` in a oneshot before it |
| polkit "not authorized" on NM | polkit rule missing wheel group | Copy `distros/fedora-kde/config/polkit/10-schema-nm.rules` |
| `/etc/resolv.conf` is a dead symlink | systemd-resolved wrote it | `rm /etc/resolv.conf && echo "nameserver 1.1.1.1" > /etc/resolv.conf` in your network oneshot |
| Plasma/GNOME hangs on settings open | Missing D-Bus interface | See D-Bus compatibility section above |
| PipeWire/PulseAudio not starting | systemd user session missing | Add autostart `.desktop` entry, or run from display manager wrapper script |
| display manager exits immediately | No seat available | Ensure elogind or schema-logind is up and answering login1 before display manager starts |
| X11/XWayland apps die with `Unable to open display` (Steam, any non-Wayland-native app) | `systemd-tmpfiles` normally creates `/tmp/.X11-unix` as `1777 root:root`; with no systemd it's missing or wrong-perm, and **an X server refuses a `/tmp/.X11-unix` without the sticky bit** — so the compositor's XWayland silently never starts and `DISPLAY` is never exported | oneshot before the display manager: `mkdir -p /tmp/.X11-unix && chown root:root /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix` (also clear stale `/tmp/.X[0-9]*-lock`). `dep=` it from the DM. On a root-fs `/tmp` (not tmpfs) the broken dir persists across reboots, so this isn't self-healing |
| flatpak/snap apps won't launch — `The name org.<app>.desktop was not provided by any .service files` | The **session** D-Bus bus computes its `.service` search dirs once at startup from `XDG_DATA_DIRS`; with no systemd user env-generator that variable is unset when the bus is born, so it never scans `…/flatpak/exports/share/dbus-1/services`. Anything later (a `plasma-workspace/env` script) runs *inside* the bus's child — too late | Export `XDG_DATA_DIRS=$HOME/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:/usr/local/share:/usr/share:/var/lib/snapd/desktop` in the env that launches the session bus, **before** the bus starts. Stopgap without re-login: symlink the `*.service` files into `~/.local/share/dbus-1/services/` (always searched regardless of `XDG_DATA_DIRS`) and `ReloadConfig` the bus |

---

## Distributions

Working configurations for specific distros and desktops live in `distros/`.

### Fedora 44 + KDE Plasma (`distros/fedora-kde/`)

Full KDE Plasma 6 desktop on Fedora 44 with schema-init as PID 1. Boots from a btrfs subvolume alongside a normal Fedora install — no repartitioning required.

**What's running:**

| Service | Role |
|---------|------|
| `udevd` | Device enumeration — required for libinput and /dev/input/event* |
| `dbus` | System bus |
| `network-up` | Loads r8152 USB ethernet module, udev settle |
| `network-manager` | Owns the network interface via NM profile |
| `polkitd` | Authorization — required for NM actions |
| `schema-logind` | Minimal `org.freedesktop.login1` D-Bus stub — restores KDE shutdown/restart buttons |
| `sddm` | Display manager (via sddm-logged wrapper, no systemd session) |
| `sound-modules` | oneshot — loads AMD Ryzen audio modules at boot |
| `bluetoothd` | Starts `bluez` daemon — registers `org.bluez`, restores KDE Bluetooth applet |
| `zram-swap` | oneshot — zstd-compressed zram swap device; replaces systemd's `zram-generator` |

Install it with the migrate wizard or the installer ISO; both use the portable service set in `distros/fedora-installer/rail/services`. `distros/fedora-kde/hosts/blakbox/` is the author's own machine, kept byte-for-byte in step with the live box by `scripts/schema-drift`. See [`distros/fedora-kde/README.md`](../distros/fedora-kde/README.md) for the layout and key fixes.

### Raspberry Pi Zero W (`distros/raspberry-pi-zero-w/`)

WiFi headless deploy on a Pi Zero W (BCM2835, armv6l, 32-bit ARM). No Ethernet, no HDMI — schema-init as PID 1, WiFi up, SSH accessible in ~50 seconds from cold boot. First ARM bare-metal target.

**Service chain:**

| Service | Role |
|---------|------|
| `udev` | Device enumeration daemon |
| `udev-trigger` | Oneshot — coldplug trigger + settle; loads brcmfmac WiFi firmware |
| `dbus` | System bus — mandatory for Pi OS wpa_supplicant |
| `wpa-supplicant` | WiFi association (config-file mode, not D-Bus mode) |
| `dhcpcd` | DHCP client, foreground (`-B`), wlan0 only |
| `sshd` | First usable interface — up when DHCP lease is held |

See [`distros/raspberry-pi-zero-w/README.md`](../distros/raspberry-pi-zero-w/README.md) for the full list of gotchas (rfkill country code, dbus privilege drop, coldplug trigger, dhcpcd forking behavior) and installation steps.

---

## Repository layout

If you're reading the source to evaluate it, start here. PID 1 is ~4,800 lines of C (the files below plus `caps.c`, `ns.c`, `landlock.c` and a few headers) with no external dependencies.

**Read these first, in this order:**

| File | Lines | What it is |
|------|-------|------------|
| `init.c` | ~2,400 | PID 1 itself. Mounts pseudo-filesystems, reaps children, runs the supervise loop, handles signals and shutdown. The spine — everything below is called from here. |
| `schema.c` / `schema.h` | ~140 | The weight-state machine. Pure state transitions; a service's "weight" is the popcount of its probe flag word. This is *the schema* — the single source of truth for what every state means. |
| `service.c` / `service.h` | ~1,650 | Parses `.svc` files, spawns services, runs the F8/F9/F6 probes, and drives the recovery → backoff → excision arc. |
| `group.c` / `group.h` | ~170 | Aggregates a `.grp` of services into one worst-case state, so a stack (network, display) promotes and fails as a unit. |

**Supporting binaries:**

| File | What it is |
|------|------------|
| `schema-ctl.c` | The CLI client. Talks to PID 1 over the `/run/schema-init.sock` UNIX socket — `schema-ctl status`, `restart`, etc. |
| `schema-subreaper.c` | ~50-line helper that sets `PR_SET_CHILD_SUBREAPER` so a service can adopt its own orphaned grandchildren instead of dumping them on PID 1. |
| `schema-journal-sink.c` | Opt-in Track B compatibility shim. Provides journald's three ingestion sockets (`/dev/log`, `/run/systemd/journal/{socket,stdout}`) and drains them to a plain logfile so foreign libsystemd/syslog software finds a journald-shaped endpoint. No journal DB, no `journalctl`. schema-init never needs it to boot. See `docs/journal-sink-design.md`. |
| `schema-systemctl.c` / `systemctl_shim.h` | The `systemctl(1)` compatibility shim. A drop-in that intercepts systemd verbs so packaged RPM/deb scriptlets succeed on a schema-init box: lifecycle verbs drive `schema-ctl`, `enable`/`preset` queue enable-intent to `/var/lib/schema-init/pending.list` for the importer (`distros/fedora-installer/migrate/schema-import.py`, which drains that queue and translates `.service` units into native `.svc`). See [Running packaged software](components.md#running-packaged-software). |
| `caps.c` / `ns.c` / `landlock.c` | Service hardening applied in the child before exec: the `keep_caps` capability bounding set and `no_new_privs` (`caps.c`), the private mount namespace behind `private_tmp` / `protect_system` / `protect_home` (`ns.c`), and the `landlock_ro` / `landlock_rw` filesystem allowlist (`landlock.c`). See [Hardening](svc-reference.md#hardening). |
| `notify.h` | The `sd_notify` readiness socket (`/run/schema-init/notify`): parsing, kernel-attested sender credentials, cgroup attribution. |
| `schema-coredump.c` / `coredump.h` | The `core_pattern` pipe helper that keeps crashes. See [Crashes](operations.md#crashes). |
| `schema-udev.c` + `*_id.h`, `udev_*.h`, `disk_links.h` | The native device manager. See [schema-udev](components.md#schema-udev). |
| `schema-dbus.c` + `sdbus_*.h` | The native D-Bus broker for the system and session buses. See [The system bus itself](components.md#the-system-bus-itself-schema-dbus). |
| `scripts/schema-snapshot`, `scripts/schema-bootok` | Boot snapshots and the boot-success guard ([docs/boot-success-guard.md](boot-success-guard.md)). |
| `schema_shm.h` | The shared-memory interface — PID 1 publishes live service state here so external tools can read it without polling the socket. |
| `schema-board.c` | Read-only board that renders every service's weight-state in its LED colour, reading the shm export above rather than the control socket — so it keeps working when the socket or the desktop is wedged. `--once` prints one frame and exits. Reads a world-readable `0644` shm segment, so unlike `schema-ctl` it **needs no root**. `--tty /dev/tty8` paints a dedicated console; note that VT switching does not currently repaint on a graphical system — see [Recovery console](operations.md#recovery-console). Increments 1–2 of the limp-mode recovery surface (`docs/superpowers/specs/2026-06-14-limp-mode-design.md`). |

**Directories:**

| Dir | What's inside |
|-----|---------------|
| `services/` | The reference service set — real `.svc` and `.grp` files for `sshd`, `dbus`, `udev`, `network-manager`, `display-manager`, and the `network-stack` / `display-stack` groups. Copy these as your starting templates. |
| `desktop/` | `schema-desktop.c` — an SDL2 live visualizer that maps `schema_shm.h` into an 8-node grid and shows every service's weight-state in real time. This is how you *watch* the state machine run. |
| `scripts/` | Build and integration tooling: `make-iso*.sh` / `make-usb.sh` / `fix-usb.sh` (bootable media), `schema-logind.py` (a logind compatibility shim), and `verify_traceability.py` (traceability check for the aspirational Daedalus IEC 62304 spec in `docs/daedalus/`). |
| `distros/` | Per-distribution profiles — `fedora-installer/rail/` (the portable Fedora KDE set), `fedora-kde/` (desktop-session glue + `hosts/blakbox/`) and `raspberry-pi-zero-w/`. |
| `docs/`, `assets/` | Documentation and images. |

Top-level: `setup.sh` (newcomer bootstrap — dep check, desktop-environment detection, GRUB entry generation) and `Makefile` (static build; see [Building](install.md#building)).
