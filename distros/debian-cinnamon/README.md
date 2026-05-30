# schema-init — Debian + Cinnamon

Runs Cinnamon desktop on Debian Bookworm under schema-init as PID 1 (no systemd).

Tested on Dell Inspiron 3542 (Intel Core i3, 4GB RAM), kernel 6.1.

## What this replaces

- systemd (PID 1) → schema-init
- systemd-logind → elogind
- systemd network management → NetworkManager + schema-network oneshot

## Installation

### 1. Install schema-init binary

```sh
sudo cp schema-init /usr/sbin/schema-init
sudo ln -s /usr/sbin/schema-init /sbin/init
```

### 2. Install service files

```sh
sudo mkdir -p /etc/schema-init/services
sudo cp services/* /etc/schema-init/services/
```

### 3. Install scripts

```sh
sudo cp scripts/schema-network /usr/local/sbin/schema-network
sudo chmod +x /usr/local/sbin/schema-network
```

### 4. Install schema-ctl

```sh
sudo cp schema-ctl /usr/local/bin/schema-ctl
```

## Services

| Service | Role |
|---------|------|
| `udev` | Device enumeration — required for input and display |
| `dbus` | System bus |
| `elogind` | Session management — replaces systemd-logind |
| `network` | oneshot — brings interfaces link-up, lets NM own DHCP |
| `network-manager` | Owns DHCP and connection management |
| `polkitd` | Authorization — required for NM and desktop actions |
| `getty-tty1` | Console login on tty1 |
| `display-manager` | LightDM |
| `sshd` | SSH access |

## Key fixes

| Problem | Fix |
|---------|-----|
| NM forks to background, schema-init sees crash | `args=--no-daemon` in network-manager.svc |
| dhclient and NM both claim the interface | schema-network only does `ip link set up` — no dhclient |
| Cinnamon won't start as root | `CINNAMON_BYPASS_ROOT_CHECK=1` in `/etc/environment` |
| /run/user not created without systemd | `/etc/X11/Xsession.d/19-no-logind` creates it at session start |
| Input devices not found by libinput | udev service must be running before display manager |

## GRUB

No `init=` kernel parameter required if `/sbin/init` is symlinked to schema-init.

To pass init explicitly:

```
linux /boot/vmlinuz-... root=LABEL=schema-root init=/usr/sbin/schema-init
```
