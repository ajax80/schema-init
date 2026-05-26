#!/bin/bash
set -e

OUT=${1:-/home/ajax80/schema-init.iso}
WORK=/tmp/schema-iso-work
MNT=$WORK/chroot
ISO=$WORK/iso

cleanup() {
    umount "$MNT/dev/pts" 2>/dev/null || true
    umount "$MNT/dev"     2>/dev/null || true
    umount "$MNT/proc"    2>/dev/null || true
    umount "$MNT/sys"     2>/dev/null || true
}
trap cleanup EXIT

echo "=== Setup ==="
rm -rf "$WORK"
mkdir -p "$MNT" "$ISO/live" "$ISO/boot/grub"

echo "=== Extracting Debian rootfs via Docker ==="
docker rm schema-iso-tmp 2>/dev/null || true
docker create --name schema-iso-tmp debian:bookworm
docker export schema-iso-tmp | tar -C "$MNT" -x
docker rm schema-iso-tmp

echo "=== Bind mounts ==="
mount --bind /dev     "$MNT/dev"
mount --bind /dev/pts "$MNT/dev/pts"
mount --bind /proc    "$MNT/proc"
mount --bind /sys     "$MNT/sys"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

echo "=== Installing packages ==="
chroot "$MNT" /bin/bash <<CHROOT
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q \
    linux-image-amd64 \
    live-boot \
    live-boot-initramfs-tools \
    isc-dhcp-client \
    iproute2 \
    busybox-static \
    passwd \
    task-cinnamon-desktop \
    lightdm \
    xserver-xorg-input-all \
    xserver-xorg-input-libinput \
    gnome-terminal \
    network-manager \
    elogind \
    libpam-elogind \
    policykit-1
echo "root:schema" | chpasswd
echo "schema-node" > /etc/hostname
pam-auth-update --enable elogind 2>/dev/null || true
CHROOT

echo "=== Installing schema-init ==="
cd /home/ajax80/projects/schema-init
gcc -std=c99 -Wall -O2 -D_GNU_SOURCE -static \
    -o "$MNT/sbin/schema-init" \
    init.c schema.c service.c -lrt
chmod +x "$MNT/sbin/schema-init"

echo "=== Writing service files ==="
mkdir -p "$MNT/etc/schema-init/services"

cat > "$MNT/etc/schema-init/services/getty-tty1.svc" <<'SVC'
name=getty-tty1
exec=/sbin/agetty
args=--autologin
args=root
args=tty1
args=linux
oneshot=0
needs_root=1
critical=1
SVC

cat > "$MNT/usr/local/sbin/schema-network" <<'NET'
#!/bin/sh
for iface in $(ls /sys/class/net); do
    [ "$iface" = "lo" ] && continue
    ip link set "$iface" up
    dhclient -1 "$iface" && break
done
NET
chmod +x "$MNT/usr/local/sbin/schema-network"

cat > "$MNT/etc/schema-init/services/network.svc" <<'SVC'
name=network
exec=/usr/local/sbin/schema-network
oneshot=1
needs_root=1
critical=0
SVC

cat > "$MNT/usr/local/sbin/schema-dbus" <<'DBUS'
#!/bin/sh
mkdir -p /run/dbus
dbus-uuidgen --ensure
exec /usr/bin/dbus-daemon --system --nofork
DBUS
chmod +x "$MNT/usr/local/sbin/schema-dbus"

cat > "$MNT/etc/schema-init/services/dbus.svc" <<'SVC'
name=dbus
exec=/usr/local/sbin/schema-dbus
oneshot=0
needs_root=1
critical=0
SVC

cat > "$MNT/usr/local/sbin/schema-udev" <<'UDEV'
#!/bin/sh
/lib/systemd/systemd-udevd &
sleep 2
udevadm trigger --action=add || true
udevadm settle --timeout=10 || true
UDEV
chmod +x "$MNT/usr/local/sbin/schema-udev"

cat > "$MNT/etc/schema-init/services/udev.svc" <<'SVC'
name=udev
exec=/usr/local/sbin/schema-udev
oneshot=1
needs_root=1
critical=0
SVC

cat > "$MNT/etc/schema-init/services/network-manager.svc" <<'SVC'
name=network-manager
exec=/usr/sbin/NetworkManager
dep=dbus
oneshot=0
needs_root=1
critical=0
SVC

cat > "$MNT/etc/schema-init/services/elogind.svc" <<'SVC'
name=elogind
exec=/usr/lib/elogind/elogind
dep=dbus
oneshot=0
needs_root=1
critical=0
SVC

printf 'name=polkit\nexec=/usr/lib/polkit-1/polkitd\ndep=dbus\noneshot=0\nneeds_root=1\ncritical=0\n' \
    > "$MNT/etc/schema-init/services/polkit.svc"

cat > "$MNT/etc/schema-init/services/display-manager.svc" <<'SVC'
name=display-manager
exec=/usr/sbin/lightdm
dep=dbus
dep=udev
dep=elogind
dep=polkit
oneshot=0
needs_root=1
critical=0
SVC

echo "=== Configuring lightdm autologin ==="
mkdir -p "$MNT/etc/lightdm/lightdm.conf.d"
cat > "$MNT/etc/lightdm/lightdm.conf.d/50-autologin.conf" <<'CONF'
[Seat:*]
autologin-user=root
autologin-user-timeout=0
user-session=cinnamon
CONF

echo "=== Desktop fixes (no-systemd) ==="
grep -q CINNAMON_BYPASS_ROOT_CHECK "$MNT/etc/environment" 2>/dev/null || \
    printf '\nCINNAMON_BYPASS_ROOT_CHECK=1\n' >> "$MNT/etc/environment"

cat > "$MNT/etc/X11/Xsession.d/19-no-logind" <<'XSESS'
#!/bin/sh
RDIR="/run/user/$(id -u)"
mkdir -p "$RDIR"
chmod 700 "$RDIR"
export XDG_RUNTIME_DIR="$RDIR"
if [ ! -S "$RDIR/bus" ]; then
    /usr/bin/dbus-daemon --session \
        --address="unix:path=$RDIR/bus" \
        --nofork --nopidfile &
    sleep 0.3
fi
export DBUS_SESSION_BUS_ADDRESS="unix:path=$RDIR/bus"
XSESS
chmod +x "$MNT/etc/X11/Xsession.d/19-no-logind"

sed -i 's/^auth.*pam_succeed_if.so user != root.*/#&/' \
    "$MNT/etc/pam.d/lightdm-autologin"

echo "=== Setting xterm as default terminal ==="
mkdir -p "$MNT/etc/dconf/db/local.d" "$MNT/etc/dconf/profile"
printf '[org/cinnamon/desktop/applications/terminal]\nexec='"'"'xterm'"'"'\nexec-arg='"'"'-e'"'"'\n' \
    > "$MNT/etc/dconf/db/local.d/00-schema"
printf 'user-db:user\nsystem-db:local\n' > "$MNT/etc/dconf/profile/user"
chroot "$MNT" dconf update 2>/dev/null || true

printf '#!/bin/sh\nexec xterm "$@"\n' > "$MNT/usr/local/bin/gnome-terminal"
chmod +x "$MNT/usr/local/bin/gnome-terminal"

echo "=== Shutdown binaries (busybox symlinks) ==="
ln -sf /bin/busybox "$MNT/sbin/poweroff"
ln -sf /bin/busybox "$MNT/sbin/halt"
ln -sf /bin/busybox "$MNT/sbin/reboot"
printf '#!/bin/sh\ncase "$1" in\n  -r|--reboot) exec /sbin/reboot ;;\n  *) exec /sbin/poweroff ;;\nesac\n' > "$MNT/sbin/shutdown"
chmod +x "$MNT/sbin/shutdown"

cat > "$MNT/etc/X11/Xsession.d/70-pulseaudio" <<'XSESS'
#!/bin/sh
if which pulseaudio >/dev/null 2>&1; then
    pulseaudio --start --exit-idle-time=-1 2>/dev/null || true
fi
XSESS
chmod +x "$MNT/etc/X11/Xsession.d/70-pulseaudio"

echo "=== Regenerating initrd with live-boot ==="
chroot "$MNT" update-initramfs -u -k all

echo "=== Copying kernel and initrd ==="
cp "$MNT"/boot/vmlinuz-*    "$ISO/live/vmlinuz"
cp "$MNT"/boot/initrd.img-* "$ISO/live/initrd.img"

echo "=== Unmounting bind mounts before squash ==="
cleanup
trap - EXIT

echo "=== Creating squashfs (this takes a while) ==="
mksquashfs "$MNT" "$ISO/live/filesystem.squashfs" \
    -e boot \
    -comp xz -noappend
echo "Squashfs size: $(du -sh $ISO/live/filesystem.squashfs | cut -f1)"

echo "=== Writing GRUB config ==="
cat > "$ISO/boot/grub/grub.cfg" <<'GRUB'
set default=0
set timeout=3

menuentry "schema-init live" {
    linux  /live/vmlinuz boot=live init=/sbin/schema-init rw quiet
    initrd /live/initrd.img
}
GRUB

echo "=== Building ISO ==="
grub2-mkrescue -o "$OUT" "$ISO"

echo "=== Done ==="
echo "ISO: $OUT  ($(du -sh $OUT | cut -f1))"
