#!/bin/sh
set -e

if [ "$(id -u)" -ne 0 ]; then
    printf "run as root\n"
    exit 1
fi

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SVC_DIR="/etc/schema-init/services"
BIN_DIR="/usr/local/bin"
KERNEL="$(uname -r)"

printf "==> building schema-init\n"
cd "$REPO"
make clean
make

printf "==> installing binaries\n"
cp schema-init /sbin/schema-init
cp schema-ctl "$BIN_DIR/schema-ctl"
chmod 755 /sbin/schema-init "$BIN_DIR/schema-ctl"

printf "==> installing services\n"
mkdir -p "$SVC_DIR"
cp "$REPO/distros/fedora-kde/services/"*.svc "$SVC_DIR/"
cp "$REPO/distros/fedora-kde/services/"*.grp "$SVC_DIR/"
cp "$REPO/services/avahi.svc"   "$SVC_DIR/"
cp "$REPO/services/chronyd.svc" "$SVC_DIR/"

printf "==> installing scripts\n"
cp "$REPO/distros/fedora-kde/scripts/mount-efi.sh"         "$BIN_DIR/mount-efi.sh"
cp "$REPO/distros/fedora-kde/scripts/mount-home-blakbox.sh" "$BIN_DIR/mount-home.sh"
cp "$REPO/distros/fedora-kde/scripts/network-up.sh"        "$BIN_DIR/network-up.sh"
cp "$REPO/distros/fedora-kde/scripts/polkitd-wrapper.sh"   "$BIN_DIR/polkitd-wrapper.sh"
cp "$REPO/distros/fedora-kde/scripts/schema-audio-start.sh" "$BIN_DIR/schema-audio-start.sh"
cp "$REPO/distros/fedora-kde/scripts/schema-logind.py"     "$BIN_DIR/schema-logind.py"
cp "$REPO/distros/fedora-kde/scripts/sddm-logged"          "$BIN_DIR/sddm-logged"
cp "$REPO/distros/fedora-kde/scripts/sound-modules.sh"     "$BIN_DIR/sound-modules.sh"
cp "$REPO/distros/fedora-kde/scripts/ollama-start.sh"      "$BIN_DIR/ollama-start.sh"
cp "$REPO/distros/fedora-kde/scripts/network-blakbox.sh" "$BIN_DIR/network-blakbox.sh"
cp "$REPO/distros/fedora-kde/scripts/seatd-run.sh"         "$BIN_DIR/seatd-run.sh"
cp "$REPO/distros/fedora-kde/scripts/plasma-session-start.sh" "$BIN_DIR/plasma-session-start.sh"
cp "$REPO/distros/fedora-kde/scripts/pipewire-run.sh"     "$BIN_DIR/pipewire-run.sh"
cp "$REPO/distros/fedora-kde/scripts/wireplumber-run.sh" "$BIN_DIR/wireplumber-run.sh"
chmod +x \
    "$BIN_DIR/mount-efi.sh" \
    "$BIN_DIR/mount-home.sh" \
    "$BIN_DIR/network-up.sh" \
    "$BIN_DIR/polkitd-wrapper.sh" \
    "$BIN_DIR/schema-audio-start.sh" \
    "$BIN_DIR/schema-logind.py" \
    "$BIN_DIR/sddm-logged" \
    "$BIN_DIR/sound-modules.sh" \
    "$BIN_DIR/ollama-start.sh" \
    "$BIN_DIR/network-blakbox.sh" \
    "$BIN_DIR/seatd-run.sh" \
    "$BIN_DIR/plasma-session-start.sh" \
    "$BIN_DIR/pipewire-run.sh" \
    "$BIN_DIR/wireplumber-run.sh"

printf "==> installing dbus policy\n"
mkdir -p /usr/share/dbus-1/system.d
cp "$REPO/distros/shared/dbus/schema-logind.conf" /usr/share/dbus-1/system.d/

printf "==> installing plymouth theme\n"
cd "$REPO/assets/plymouth-theme"
python3 generate-frames.py
THEME_DIR="/usr/share/plymouth/themes/airzdowne"
mkdir -p "$THEME_DIR"
cp airzdowne.plymouth "$THEME_DIR/"
cp ./*.png "$THEME_DIR/" 2>/dev/null || true
plymouth-set-default-theme airzdowne
printf "==> rebuilding initramfs (this takes 1-2 minutes)\n"
dracut --force "/boot/initramfs-${KERNEL}.img" "$KERNEL"

printf "==> adding GRUB entry\n"
grubby \
    --add-kernel="/boot/vmlinuz-${KERNEL}" \
    --initrd="/boot/initramfs-${KERNEL}.img" \
    --title="Fedora (schema-init)" \
    --args="root=UUID=90557be5-57a8-4ff5-bc32-e1bc83be6d75 ro rootflags=subvol=root quiet splash split_lock_detect=off pci=pcie_bus_perf nowatchdog nmi_watchdog=0 nvidia-drm.modeset=1 init=/sbin/schema-init"

printf "\n==> done\n"
printf "Reboot and select 'Fedora (schema-init)' from the GRUB menu.\n"
printf "Your default systemd entry is unchanged.\n"
