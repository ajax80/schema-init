# schema-init — Fedora + Cinnamon

Runs Cinnamon desktop on Fedora 44 under schema-init as PID 1 (no systemd).

## What this replaces

- systemd (PID 1) → schema-init
- LightDM session management → `/usr/sbin/lightdm` service
- systemd user units (pipewire, wireplumber) → Cinnamon autostart
- systemd-resolved → static /etc/resolv.conf written at boot

## Prerequisites

### User groups
Add your user to the required device groups:
```
sudo usermod -a -G video,input,audio,wheel YOUR_USER
```
Log out and back in (or reboot) for groups to take effect.

### Btrfs layout (GreyBox reference)
- Root subvolume: `root` or `schema` depending on your setup.
- Home subvolume: `home` subvolume, mounted by mount-home.sh at boot.

If your layout differs, edit `scripts/mount-home.sh` and `services/mount-home.svc`.

## Installation

### 1. Install schema-init binary
```
sudo cp schema-init /sbin/init
```

### 2. Install service files
```
sudo mkdir -p /etc/schema-init/services
sudo cp services/* /etc/schema-init/services/
```

### 3. Install scripts
```
sudo cp scripts/network-up.sh /usr/local/bin/
sudo cp scripts/mount-home.sh /usr/local/bin/
sudo cp scripts/sound-modules.sh /usr/local/bin/
sudo cp scripts/schema-audio-start.sh /usr/local/bin/
sudo cp scripts/schema-logind.py /usr/local/bin/
sudo chmod +x /usr/local/bin/network-up.sh \
               /usr/local/bin/mount-home.sh \
               /usr/local/bin/sound-modules.sh \
               /usr/local/bin/schema-audio-start.sh \
               /usr/local/bin/schema-logind.py
```

### 4. Install user configs (as YOUR_USER)
```
mkdir -p ~/.config/autostart
cp config/autostart/schema-audio.desktop ~/.config/autostart/
```

### 5. Install polkit rule
```
sudo cp config/polkit/10-schema-nm.rules /etc/polkit-1/rules.d/
```

## Key fixes explained

| Problem | Fix |
|---------|-----|
| Touchpad dead at LightDM login | `udev-settle` oneshot runs `udevadm settle` before LightDM starts, preventing input race conditions. |
| /etc/resolv.conf dead symlink | `network-up.sh` removes symlink, writes nameservers. |
| PipeWire not starting | Cinnamon autostart via `schema-audio.desktop`. |
| NM "not authorized" | polkit rule granting wheel group NM control. |
| KDE/Cinnamon System Settings hangs | `schema-logind` registers `org.freedesktop.systemd1` and `org.freedesktop.hostname1` stubs to prevent D-Bus timeouts. |

## Network

Static IP configured in `scripts/network-up.sh`. Edit the IP, gateway, and interface to match your setup.
