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

The session and audio services (`sddm-logged`, `pipewire-run.sh`, etc.) run the desktop as a specific user. They read `SCHEMA_USER`/`SCHEMA_UID` from `/etc/schema-init/user.conf`, defaulting to `ajax80`/`1000` if absent. Point them at your account:
```
printf 'SCHEMA_USER=%s\nSCHEMA_UID=%s\n' "$USER" "$(id -u)" | sudo tee /etc/schema-init/user.conf
```
`install-blakbox.sh` writes this file automatically from the invoking user.

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
| plasmashell ~30% idle CPU (ksycoca rebuild loop) | KService gates cache validation on libsystemd `sd_booted()` = `access("/run/systemd/system/")`. With no systemd that dir is absent, so it never confirms the cache is fresh and self-feeds an in-process rebuild loop (rebuild → `databaseChanged` → AppsModel refresh → rebuild). `scripts/mock_sd.c` → `/usr/local/lib/mock_sd.so` is an `LD_PRELOAD` shim overriding `access`/`stat`/`statx` to report that dir exists, applied to plasmashell only via the `plasmashell-shim` wrapper (used by both `~/.config/autostart/org.kde.plasmashell.desktop` and `plasma-session-start.sh`). Drops to 0% idle. A session-bus `systemd1` D-Bus mock was tried first and did **not** work — the gate is the filesystem check, not D-Bus. Tradeoff: ksycoca no longer auto-polls, so run `kbuildsycoca6` (from a full session shell, with flatpak paths in `XDG_DATA_DIRS`) after installing new apps |
| Plymouth black screen on AMD GPU | `script` plugin fails on AMD Picasso/Raven DRM; use `ModuleName=two-step` with pre-rendered frames |
| Boot shows `^[[3~` escape sequences | Plymouth restores TTY echo on exit; `sddm-logged` runs `stty -echo` both before and immediately after `plymouth --wait quit`, then `tcflush` + `clear` on tty1 |
| KDE Connect not discovered on LAN | `avahi-daemon` not running; `services/avahi.svc` starts it after dbus |
| Clock wrong after reboot | `chronyd` not running; `services/chronyd.svc` starts it after network-manager |
| KDE Bluetooth applet dead, no controller | `bluetoothd` not running so `org.bluez` never registers on the system bus; `services/bluetoothd.svc` starts `/usr/libexec/bluetooth/bluetoothd -n` after dbus. Loadable live with `schema-ctl add` — no reboot |
| Xbox One/Series controller won't pair over BT | Kernel ERTM (Enhanced Re-Transmission Mode); disable it: `echo "options bluetooth disable_ertm=1" > /etc/modprobe.d/bluetooth.conf` and `echo 1 > /sys/module/bluetooth/parameters/disable_ertm` to apply live |

## Audio hardware

Tested on AMD Ryzen with `snd_hda_intel`, `snd_acp3x_rn`, `snd_rn_pci_acp3x`.
Edit `scripts/sound-modules.sh` for other hardware.

## Network

Static IP configured in `scripts/network-up.sh`. Edit the IP, gateway, and interface to match your setup. USB ethernet (r8152) modprobed automatically.
