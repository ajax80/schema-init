# schema-init — Fedora + KDE Plasma

Runs KDE Plasma 6 on Fedora 44 under schema-init as PID 1 (no systemd).

## What this replaces

- systemd (PID 1) → schema-init
- SDDM session management → sddm-logged wrapper
- systemd user units (pipewire, wireplumber) → KDE autostart
- systemd-resolved → static /etc/resolv.conf written at boot

## Prerequisites

### User groups
Add your user to the required device groups:
```
sudo usermod -a -G video,input,audio,wheel YOUR_USER
```
Log out and back in (or reboot) for groups to take effect.

### Btrfs layout (GreyBox reference)
- Root subvolume: `schema` on UUID d595d899-40f1-4c8b-9310-402fe56c0422
- Home subvolume: `home` on same UUID, mounted by mount-home.sh at boot

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
sudo cp scripts/sddm-logged /usr/local/bin/
sudo chmod +x /usr/local/bin/network-up.sh \
               /usr/local/bin/mount-home.sh \
               /usr/local/bin/sound-modules.sh \
               /usr/local/bin/schema-audio-start.sh \
               /usr/local/bin/sddm-logged
```

### 4. Install user configs (as YOUR_USER)
```
mkdir -p ~/.config/autostart
cp config/ksplashrc ~/.config/
cp config/plasma-session.conf ~/.config/
cp config/autostart/schema-audio.desktop ~/.config/autostart/
```

### 5. Install polkit rule
```
sudo cp config/polkit/10-schema-nm.rules /etc/polkit-1/rules.d/
```

### 6. Install Plymouth boot theme
```
sudo mkdir -p /usr/share/plymouth/themes/airzdowne
sudo cp assets/plymouth-theme/airzdowne.plymouth /usr/share/plymouth/themes/airzdowne/
sudo cp assets/plymouth-theme/logo.png /usr/share/plymouth/themes/airzdowne/
```

Generate the 30-frame breathing animation (requires `pillow`):
```
pip install pillow
python3 assets/plymouth-theme/generate-frames.py /usr/share/plymouth/themes/airzdowne/
```

Copy password prompt graphics from the bundled spinner theme:
```
sudo cp /usr/share/plymouth/themes/spinner/{bullet,lock,entry,capslock}.png \
        /usr/share/plymouth/themes/airzdowne/
```

Set as default and rebuild initramfs:
```
sudo plymouth-set-default-theme airzdowne
sudo dracut -f --regenerate-all
```

Ensure `quiet rhgb` is present in your kernel command line:
```
sudo grubby --update-kernel=ALL --args="quiet rhgb"
```

## Key fixes explained

| Problem | Fix |
|---------|-----|
| kwin_wayland needs DRM | User in `video` group + `LIBSEAT_BACKEND=direct` |
| KSplash crashes (nested Wayland) | `ksplashrc` Engine=none |
| Plasma hangs on systemd user units | `plasma-session.conf` systemdBoot=false |
| /etc/resolv.conf dead symlink | `network-up.sh` removes symlink, writes nameservers |
| PipeWire not starting | KDE autostart via `schema-audio.desktop` |
| NM "not authorized" | polkit rule granting wheel group NM control |
| AMD Ryzen audio modules not loaded | `sound-modules.svc` oneshot at boot |
| KDE System Settings hangs 25s on open | `schema-logind` registers `org.freedesktop.systemd1` stub — `GetUnitFileState` returns immediately instead of timing out waiting for systemd activation |
| About This System hangs 25s | `schema-logind` registers `org.freedesktop.hostname1` stub — hostname, OS name, hardware vendor/model returned instantly |
| Plymouth black screen on AMD GPU | `script` plugin fails on AMD Picasso/Raven DRM; use `ModuleName=two-step` with pre-rendered frames |
| Boot shows `^[[3~` escape sequences | Plymouth restores TTY echo on exit; `sddm-logged` runs `stty -echo` both before and immediately after `plymouth --wait quit`, then `tcflush` + `clear` on tty1 |
| KDE Connect not discovered on LAN | `avahi-daemon` not running; `services/avahi.svc` starts it after dbus |
| Clock wrong after reboot | `chronyd` not running; `services/chronyd.svc` starts it after network-manager |

## Audio hardware

Tested on AMD Ryzen with `snd_hda_intel`, `snd_acp3x_rn`, `snd_rn_pci_acp3x`.
Edit `scripts/sound-modules.sh` for other hardware.

## Network

Static IP configured in `scripts/network-up.sh`. Edit the IP, gateway, and interface to match your setup. USB ethernet (r8152) modprobed automatically.
