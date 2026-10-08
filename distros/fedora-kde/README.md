# schema-init — Fedora + KDE Plasma

Runs KDE Plasma 6 on Fedora 44 under schema-init as PID 1 (no systemd).

## What this replaces

- systemd (PID 1) → schema-init
- SDDM session management → schema-plasma-autologin.sh wrapper
- systemd user units (pipewire, wireplumber) → KDE autostart
- systemd-resolved → static /etc/resolv.conf written at boot

## Layout

- `scripts/`, `config/`: the desktop-session pieces (autologin, plasmashell shim, autostart runner, env hooks, seatd and wireplumber helpers). The RPM, the migrate wizard and the installer ISO install them from here.
- `hosts/blakbox/`: one real machine's whole service set (`services/`) and the host-only scripts those services exec (`scripts/`). It is not a profile for other machines: it mounts specific disks by UUID and runs that box's own services.
- The portable service set is `distros/fedora-installer/rail/services`. On Fedora KDE, install with the migrate wizard (`sudo schema-migrate`) or the installer ISO.

## Reproducing blakbox

```
sudo distros/fedora-kde/install-blakbox.sh   # services + the scripts they exec; old set backed up to /root
scripts/schema-drift                         # repo vs live; lists every difference, exit 1 if any
```

New services load at the next boot: PID 1 refuses to reload a `.svc` that changed since boot. When blakbox is changed by hand, copy the change into `hosts/blakbox/` and rerun `schema-drift` until it reports no drift. Paths in `hosts/blakbox/drift-ignore` (private scripts) and paths an RPM owns are not tracked.

## Manual desktop extras

### User groups
Add your user to the required device groups:
```
sudo usermod -a -G video,input,audio,wheel YOUR_USER
```
Log out and back in (or reboot) for groups to take effect.

### User configs (as YOUR_USER)
```
mkdir -p ~/.config/autostart
cp config/ksplashrc ~/.config/
cp config/plasma-session.conf ~/.config/
cp config/autostart/schema-audio.desktop ~/.config/autostart/
```

### Polkit rule
```
sudo cp config/polkit/10-schema-nm.rules /etc/polkit-1/rules.d/
```

### Plymouth boot theme
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
| Blank screen on NVIDIA — kwin starts (`wayland-0` socket appears after ~25s) but no display output | NVIDIA proprietary driver with `nvidia-drm.modeset=1` requires **atomic** modesetting. **Never set `KWIN_DRM_NO_AMS=1`** — it's a common mouse-latency tweak, but disabling atomic modeset on NVIDIA KMS gives you a running compositor with zero output. The safe latency knob is `__GL_SYNC_TO_VBLANK=0`, which helps and does no harm |
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
| Boot shows `^[[3~` escape sequences | Plymouth restores TTY echo on exit; `schema-plasma-autologin.sh` runs `stty -echo` and clears tty1 before quitting Plymouth |
| KDE Connect not discovered on LAN | `avahi-daemon` not running; `avahi.svc` starts it after dbus |
| Clock wrong after reboot | `chronyd` not running; `chronyd.svc` starts it after network-manager |
| KDE Bluetooth applet dead, no controller | `bluetoothd` not running so `org.bluez` never registers on the system bus; `bluetoothd.svc` starts `/usr/libexec/bluetooth/bluetoothd -n` after dbus. Loadable live with `schema-ctl add` — no reboot |
| Xbox One/Series controller won't pair over BT | Kernel ERTM (Enhanced Re-Transmission Mode); disable it: `echo "options bluetooth disable_ertm=1" > /etc/modprobe.d/bluetooth.conf` and `echo 1 > /sys/module/bluetooth/parameters/disable_ertm` to apply live |
| Periodic stutter/hitching under memory pressure (e.g. gaming) | No systemd means `zram-generator` never runs, so the system boots with **zero swap**. Under RAM pressure the kernel thrashes — discarding and re-reading page cache from disk (high iowait, processes stuck in `D` state), producing a stutter every few seconds. `zram-swap.svc` creates a zstd-compressed zram swap device at boot (~3–4x compression), restoring the headroom systemd would normally provide |

## Audio hardware

Tested on AMD Ryzen with `snd_hda_intel`, `snd_acp3x_rn`, `snd_rn_pci_acp3x`.
Edit `scripts/sound-modules.sh` for other hardware.

## Network

Static IP configured in `scripts/network-up.sh`. Edit the IP, gateway, and interface to match your setup. USB ethernet (r8152) modprobed automatically.
