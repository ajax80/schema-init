#!/bin/bash
set -e

DEV=${1:-/dev/sde}
MNT=/mnt/schema-usb

echo "=== Partitioning $DEV ==="
parted -s "$DEV" mklabel gpt
# 1MB BIOS Boot partition — lets GRUB embed on GPT/legacy BIOS
parted -s "$DEV" mkpart bios    1MiB    2MiB
parted -s "$DEV" set 1 bios_grub on
# EFI System Partition
parted -s "$DEV" mkpart esp fat32  2MiB  258MiB
parted -s "$DEV" set 2 esp on
# Root
parted -s "$DEV" mkpart root ext4 258MiB 100%
partprobe "$DEV"
sleep 1

echo "=== Formatting ==="
mkfs.fat -F32 "${DEV}2"
mkfs.ext4 -F -L schema-root "${DEV}3"

echo "=== Mounting ==="
mkdir -p "$MNT"
mount "${DEV}3" "$MNT"
mkdir -p "$MNT/boot/efi"
mount "${DEV}2" "$MNT/boot/efi"

echo "=== Extracting Debian rootfs via Docker ==="
docker create --name schema-usb-tmp debian:bookworm
docker export schema-usb-tmp | tar -C "$MNT" -x
docker rm schema-usb-tmp

echo "=== Bind mounts ==="
mount --bind /dev     "$MNT/dev"
mount --bind /dev/pts "$MNT/dev/pts"
mount --bind /proc    "$MNT/proc"
mount --bind /sys     "$MNT/sys"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

echo "=== Installing packages inside chroot ==="
chroot "$MNT" /bin/bash <<CHROOT
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q \
    linux-image-amd64 \
    grub-efi-amd64-bin \
    grub-pc-bin \
    shim-signed \
    util-linux \
    isc-dhcp-client \
    iproute2 \
    busybox-static \
    passwd \
    task-cinnamon-desktop \
    lightdm

echo "root:schema" | chpasswd
echo "schema-node" > /etc/hostname

cat > /etc/fstab <<'FSTAB'
LABEL=schema-root  /        ext4  defaults  0 1
FSTAB

cat > /etc/default/grub <<'GRUB'
GRUB_DEFAULT=0
GRUB_TIMEOUT=3
GRUB_DISTRIBUTOR="schema-init"
GRUB_CMDLINE_LINUX_DEFAULT="rw init=/sbin/schema-init"
GRUB_CMDLINE_LINUX=""
GRUB

grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=schema-init --removable
grub-install --target=i386-pc "$DEV"
update-grub
CHROOT

echo "=== Installing schema-init ==="
cd /home/ajax80/projects/schema-init
make clean
gcc -std=c99 -Wall -O2 -D_GNU_SOURCE -static \
    -o "$MNT/sbin/schema-init" \
    init.c schema.c service.c -lrt
chmod +x "$MNT/sbin/schema-init"
echo "Binary: $(file $MNT/sbin/schema-init)"

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

cat > "$MNT/etc/schema-init/services/dbus.svc" <<'SVC'
name=dbus
exec=/usr/bin/dbus-daemon
args=--system
args=--nofork
oneshot=0
needs_root=1
critical=0
SVC

cat > "$MNT/etc/schema-init/services/display-manager.svc" <<'SVC'
name=display-manager
exec=/usr/sbin/lightdm
dep=dbus
oneshot=0
needs_root=1
critical=0
SVC

echo "=== Unmounting ==="
umount "$MNT/dev/pts"
umount "$MNT/dev"
umount "$MNT/proc"
umount "$MNT/sys"
umount "$MNT/boot/efi"
umount "$MNT"

echo "=== Done — USB ready at $DEV ==="
