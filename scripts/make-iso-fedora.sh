#!/bin/bash
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"

OUT=${1:-${HOME}/schema-init-fedora.iso}
WORK=${HOME}/schema-fedora-work
MNT=$WORK/chroot
ISO=$WORK/iso
LABEL=SCHEMA_FEDORA
LIVEUSER=live

cleanup() {
    umount "$MNT/dev/pts" 2>/dev/null || true
    umount "$MNT/dev"     2>/dev/null || true
    umount "$MNT/proc"    2>/dev/null || true
    umount "$MNT/sys"     2>/dev/null || true
}
trap cleanup EXIT

printf "=== Setup ===\n"
rm -rf "$WORK"
mkdir -p "$MNT" "$ISO/LiveOS" "$ISO/images/pxeboot" "$ISO/boot/grub"

printf "=== Extracting Fedora rootfs via Docker ===\n"
docker rm schema-fedora-tmp 2>/dev/null || true
docker create --name schema-fedora-tmp fedora:40
docker export schema-fedora-tmp | tar -C "$MNT" -x
docker rm schema-fedora-tmp

printf "=== Bind mounts ===\n"
mount --bind /dev     "$MNT/dev"
mount --bind /dev/pts "$MNT/dev/pts"
mount --bind /proc    "$MNT/proc"
mount --bind /sys     "$MNT/sys"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

printf "=== Installing packages ===\n"
chroot "$MNT" dnf install -y \
    kernel \
    dracut-live \
    iproute \
    NetworkManager \
    polkit \
    pipewire \
    wireplumber \
    pipewire-pulseaudio \
    plasma-workspace \
    kwin \
    dbus-daemon \
    mesa-dri-drivers \
    plasma-pa \
    sddm \
    dolphin \
    konsole \
    sudo \
    passwd \
    hostname \
    which \
    --setopt=install_weak_deps=False

printf "=== Creating live user ===\n"
chroot "$MNT" useradd -m -u 1000 -G wheel,video,input,audio "$LIVEUSER"
chroot "$MNT" passwd -d "$LIVEUSER"
printf '%s ALL=(ALL) NOPASSWD: ALL\n' "$LIVEUSER" > "$MNT/etc/sudoers.d/$LIVEUSER"
chmod 440 "$MNT/etc/sudoers.d/$LIVEUSER"
printf 'schema-live\n' > "$MNT/etc/hostname"

printf "=== Dropping systemd from nsswitch.conf ===\n"
# schema-init calls getgrnam("schema") on every boot and getpwnam() for any .svc
# with user=. Fedora's default nsswitch lists `systemd` as a source, so glibc
# dlopens libnss_systemd.so.2 -- which, with no systemd running (schema-init IS
# pid 1), null-derefs and takes PID 1 with it:
#   schema-init[1]: segfault at 28 ... in libnss_systemd.so.2
#   Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000008b
# Nothing in this image runs systemd at runtime, so the module is pure liability.
sed -i -E 's/^(passwd|group|shadow):(.*)\[SUCCESS=merge\] systemd/\1:\2/; s/^(passwd|group|shadow):(.*) systemd/\1:\2/' \
    "$MNT/etc/nsswitch.conf"
grep -E '^(passwd|group|shadow):' "$MNT/etc/nsswitch.conf"

printf "=== Installing schema-init ===\n"
# Build INSIDE the chroot. Building on the host linked a static binary against
# the host's glibc (F44, 2.43) which then ran in the F40 userland (2.39) -- the
# linker warns about exactly this for getpwnam/getgrnam/initgroups -- and PID 1
# died with SIGFPE, panicking the kernel with "Attempted to kill init!".
mkdir -p "$MNT/build"
cp "$REPO"/*.c "$REPO"/*.h "$MNT/build/"
chroot "$MNT" dnf install -y gcc glibc-static --setopt=install_weak_deps=False
chroot "$MNT" sh -c 'cd /build && gcc -std=c99 -Wall -O2 -D_GNU_SOURCE -static \
    -o /sbin/schema-init init.c schema.c service.c group.c -lrt'
rm -rf "$MNT/build"
chmod +x "$MNT/sbin/schema-init"
ln -sf /sbin/schema-init "$MNT/sbin/init"

printf "=== Writing service files ===\n"
mkdir -p "$MNT/etc/schema-init/services"
cp "$REPO"/distros/fedora-kde/services/*.svc \
   "$REPO"/distros/fedora-kde/services/*.grp \
   "$MNT/etc/schema-init/services/"

sed -i "s/ajax80/$LIVEUSER/g" "$MNT/etc/schema-init/services/sddm.svc" 2>/dev/null || true

# distros/fedora-kde/services is blakbox's own config, not a portable profile.
# Drop the host-specific units and the deps that reference them, or sddm blocks
# forever on services that can never come up here. x11-tmpfiles has no .svc at
# all -- a dangling dep that stalls the whole display-stack chain.
SVCDIR="$MNT/etc/schema-init/services"
for hostsvc in mount-ocean mount-ocean-drives mount-efi network-blakbox \
               nordvpnd tailscaled ollama greybox-audio nvidia-modules; do
    rm -f "$SVCDIR/$hostsvc.svc"
done
sed -i -E '/^dep=(nvidia-modules|x11-tmpfiles|mount-ocean|mount-ocean-drives|mount-efi|network-blakbox|nordvpnd|tailscaled|ollama|greybox-audio)$/d' \
    "$SVCDIR"/*.svc
sed -i -E '/^member=(nvidia-modules|mount-ocean|mount-ocean-drives|mount-efi|network-blakbox|nordvpnd|tailscaled|ollama|greybox-audio)$/d' \
    "$SVCDIR"/*.grp
printf 'live profile services: '; ls "$SVCDIR" | tr '\n' ' '; printf '\n'
printf 'sddm deps: '; grep '^dep=' "$SVCDIR/sddm.svc" | tr '\n' ' '; printf '\n'

printf "=== Installing schema-logind ===\n"
# services/schema-logind.svc points at /usr/local/bin/schema-logind.py; without
# this the service starts and immediately fails, the login1 name is never taken,
# and KWin silently falls back to a no-op session with no VT management.
chroot "$MNT" dnf install -y python3-dbus python3-gobject \
    --setopt=install_weak_deps=False
cp "$REPO"/scripts/schema-logind.py "$MNT/usr/local/bin/"
chmod +x "$MNT/usr/local/bin/schema-logind.py"

printf "=== Installing scripts ===\n"
cp "$REPO"/distros/fedora-kde/scripts/sddm-logged \
   "$REPO"/distros/fedora-kde/scripts/mount-home.sh \
   "$REPO"/distros/fedora-kde/scripts/sound-modules.sh \
   "$REPO"/distros/fedora-kde/scripts/schema-audio-start.sh \
   "$REPO"/distros/fedora-kde/scripts/schema-udev-trigger.sh \
   "$REPO"/distros/fedora-kde/scripts/schema-hostname.sh \
   "$MNT/usr/local/bin/"
chmod +x "$MNT/usr/local/bin/sddm-logged" \
          "$MNT/usr/local/bin/sound-modules.sh" \
          "$MNT/usr/local/bin/schema-audio-start.sh" \
          "$MNT/usr/local/bin/schema-udev-trigger.sh" \
          "$MNT/usr/local/bin/schema-hostname.sh"

printf "#!/bin/sh\n# live ISO: home is on rootfs, no separate mount needed\nexit 0\n" \
    > "$MNT/usr/local/bin/mount-home.sh"
chmod +x "$MNT/usr/local/bin/mount-home.sh"

LIVEUID=$(chroot "$MNT" id -u "$LIVEUSER" 2>/dev/null || printf '1000')
sed -i "s|ajax80|$LIVEUSER|g; s|/run/user/1000|/run/user/$LIVEUID|g" \
    "$MNT/usr/local/bin/sddm-logged"

printf "=== Network (DHCP for live) ===\n"
NETSCRIPT="$MNT/usr/local/bin/network-up.sh"
printf '#!/bin/sh\n' > "$NETSCRIPT"
printf 'exec >> /var/log/network-up.log 2>&1\n' >> "$NETSCRIPT"
printf 'printf "network-up start: %%s\\n" "$(date)"\n' >> "$NETSCRIPT"
printf 'udevadm trigger --subsystem-match=net 2>/dev/null || true\n' >> "$NETSCRIPT"
printf 'udevadm settle --timeout=5 2>/dev/null || true\n' >> "$NETSCRIPT"
printf 'rm -f /etc/resolv.conf\n' >> "$NETSCRIPT"
printf 'printf "nameserver 8.8.8.8\\nnameserver 1.1.1.1\\n" > /etc/resolv.conf\n' >> "$NETSCRIPT"
printf 'IFACE=$(ip -o link show | awk -F'"'"': '"'"' '"'"'{print $2}'"'"' | grep -v '"'"'^lo$'"'"' | grep -v '"'"'^wl'"'"' | head -1)\n' >> "$NETSCRIPT"
printf '[ -z "$IFACE" ] && exit 0\n' >> "$NETSCRIPT"
printf 'ip link set "$IFACE" up 2>&1\n' >> "$NETSCRIPT"
printf 'dhclient -1 "$IFACE" 2>&1 || true\n' >> "$NETSCRIPT"
printf 'printf "network-up done\\n"\n' >> "$NETSCRIPT"
printf 'exit 0\n' >> "$NETSCRIPT"
chmod +x "$NETSCRIPT"

printf "=== KDE user config ===\n"
UHOME="$MNT/home/$LIVEUSER"
mkdir -p "$UHOME/.config/autostart"
chown -R 1000:1000 "$UHOME"
cp "$REPO"/distros/fedora-kde/config/ksplashrc \
   "$REPO"/distros/fedora-kde/config/plasma-session.conf \
   "$UHOME/.config/"
cp "$REPO"/distros/fedora-kde/config/autostart/schema-audio.desktop \
   "$UHOME/.config/autostart/"
chown -R 1000:1000 "$UHOME/.config"

printf "=== Polkit rule ===\n"
mkdir -p "$MNT/etc/polkit-1/rules.d"
cp "$REPO"/distros/fedora-kde/config/polkit/10-schema-nm.rules \
   "$MNT/etc/polkit-1/rules.d/"

printf "=== Shutdown wrappers ===\n"
for dir in "$MNT/sbin" "$MNT/usr/sbin"; do
    mkdir -p "$dir"
    rm -f "$dir/poweroff" "$dir/halt" "$dir/reboot" "$dir/shutdown"
    printf '#!/bin/sh\nkill -TERM 1\n' > "$dir/poweroff"
    printf '#!/bin/sh\nkill -TERM 1\n' > "$dir/halt"
    printf '#!/bin/sh\nkill -INT 1\n'  > "$dir/reboot"
    printf '#!/bin/sh\ncase "$1" in\n  -r|--reboot) kill -INT 1 ;;\n  *) kill -TERM 1 ;;\nesac\n' > "$dir/shutdown"
    chmod +x "$dir/poweroff" "$dir/halt" "$dir/reboot" "$dir/shutdown"
done

printf "=== Dracut live config ===\n"
mkdir -p "$MNT/etc/dracut.conf.d"
printf 'add_dracutmodules+=" dmsquash-live "\n' > "$MNT/etc/dracut.conf.d/live.conf"
printf 'omit_dracutmodules+=" systemd-pcrphase "\n' >> "$MNT/etc/dracut.conf.d/live.conf"
printf 'compress="xz"\n' >> "$MNT/etc/dracut.conf.d/live.conf"

printf "=== Regenerating initrd ===\n"
KVER=$(ls "$MNT/usr/lib/modules/" | head -1)
chroot "$MNT" dracut --force --no-hostonly --omit "systemd-pcrphase" \
    --kver "$KVER" /boot/initramfs-live.img

printf "=== Copying kernel and initrd ===\n"
cp "$MNT/usr/lib/modules/$KVER/vmlinuz" "$ISO/images/pxeboot/vmlinuz"
cp "$MNT/boot/initramfs-live.img"       "$ISO/images/pxeboot/initrd.img"

printf "=== Unmounting before squash ===\n"
cleanup
trap - EXIT

printf "=== Creating squashfs ===\n"
mksquashfs "$MNT" "$ISO/LiveOS/filesystem.squashfs" \
    -e boot \
    -comp xz -noappend
printf "Squashfs size: %s\n" "$(du -sh "$ISO/LiveOS/filesystem.squashfs" | cut -f1)"

printf "=== Writing GRUB config ===\n"
printf 'set default=0\nset timeout=5\n\n' > "$ISO/boot/grub/grub.cfg"
printf 'menuentry "schema-init Fedora KDE live" {\n' >> "$ISO/boot/grub/grub.cfg"
printf '    linux  /images/pxeboot/vmlinuz root=live:CDLABEL=%s rd.live.image rd.live.squashimg=filesystem.squashfs rw init=/sbin/schema-init console=tty0 console=ttyS0,115200\n' "$LABEL" >> "$ISO/boot/grub/grub.cfg"
printf '    initrd /images/pxeboot/initrd.img\n' >> "$ISO/boot/grub/grub.cfg"
printf '}\n' >> "$ISO/boot/grub/grub.cfg"

printf "=== Building ISO ===\n"
grub2-mkrescue -o "$OUT" "$ISO" -- -volid "$LABEL"

printf "=== Done ===\n"
printf "ISO: %s  (%s)\n" "$OUT" "$(du -sh "$OUT" | cut -f1)"
