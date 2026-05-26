#!/bin/bash
set -e

DEV=${1:-/dev/sde}
MNT=/mnt/schema-usb

echo "=== Mounting ==="
mkdir -p "$MNT"
mount "${DEV}2" "$MNT"
mount "${DEV}1" "$MNT/boot/efi"
mount --bind /dev     "$MNT/dev"
mount --bind /dev/pts "$MNT/dev/pts"
mount --bind /proc    "$MNT/proc"
mount --bind /sys     "$MNT/sys"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

echo "=== Building schema-init static ==="
cd /home/ajax80/projects/schema-init
make clean
gcc -std=c99 -Wall -O2 -D_GNU_SOURCE -static \
    -o "$MNT/sbin/schema-init" \
    init.c schema.c service.c -lrt
chmod +x "$MNT/sbin/schema-init"
echo "Binary: $(file $MNT/sbin/schema-init)"

echo "=== Installing packages ==="
chroot "$MNT" /bin/bash <<'CHROOT'
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -q isc-dhcp-client iproute2 2>/dev/null || true
apt-get install -y task-cinnamon-desktop lightdm 2>/dev/null || true
CHROOT

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

cat > "$MNT/etc/schema-init/services/display-manager.svc" <<'SVC'
name=display-manager
exec=/usr/sbin/lightdm
dep=network
oneshot=0
needs_root=1
critical=0
SVC

echo "=== Fixing GRUB config ==="
chroot "$MNT" /bin/bash <<'CHROOT'
cat > /etc/default/grub <<'GRUB'
GRUB_DEFAULT=0
GRUB_TIMEOUT=3
GRUB_DISTRIBUTOR="schema-init"
GRUB_CMDLINE_LINUX_DEFAULT="rw init=/sbin/schema-init"
GRUB_CMDLINE_LINUX=""
GRUB
update-grub
CHROOT

echo "=== Unmounting ==="
umount "$MNT/dev/pts"
umount "$MNT/dev"
umount "$MNT/proc"
umount "$MNT/sys"
umount "$MNT/boot/efi"
umount "$MNT"

echo "=== USB ready ==="
