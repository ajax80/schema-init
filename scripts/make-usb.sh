#!/bin/bash
set -e

DEV=${1:-/dev/sde}
MNT=/mnt/schema-usb

echo "=== Partitioning $DEV ==="
parted -s "$DEV" mklabel gpt
parted -s "$DEV" mkpart esp  fat32  1MiB  256MiB
parted -s "$DEV" set 1 esp on
parted -s "$DEV" mkpart root ext4  256MiB 100%
partprobe "$DEV"
sleep 1

echo "=== Formatting ==="
mkfs.fat -F32 "${DEV}1"
mkfs.ext4 -F -L schema-root "${DEV}2"

echo "=== Mounting ==="
mkdir -p "$MNT"
mount "${DEV}2" "$MNT"
mkdir -p "$MNT/boot/efi"
mount "${DEV}1" "$MNT/boot/efi"

echo "=== Extracting Debian rootfs via Docker ==="
docker create --name schema-usb-tmp debian:bookworm
docker export schema-usb-tmp | tar -C "$MNT" -x
docker rm schema-usb-tmp

echo "=== Bind mounts ==="
mount --bind /dev     "$MNT/dev"
mount --bind /dev/pts "$MNT/dev/pts"
mount --bind /proc    "$MNT/proc"
mount --bind /sys     "$MNT/sys"

echo "=== Installing kernel + bootloader inside chroot ==="
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

chroot "$MNT" /bin/bash <<'CHROOT'
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q \
    linux-image-amd64 \
    grub-efi-amd64-bin \
    grub-pc-bin \
    shim-signed \
    util-linux \
    busybox-static \
    passwd

# root password: schema
echo "root:schema" | chpasswd

# fstab
cat > /etc/fstab <<'FSTAB'
LABEL=schema-root  /        ext4  defaults  0 1
FSTAB

# hostname
echo "schema-node" > /etc/hostname

# GRUB: add init= to kernel cmdline
sed -i 's|GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT="quiet init=/sbin/schema-init"|' /etc/default/grub
echo 'GRUB_TIMEOUT=3' >> /etc/default/grub

# install GRUB for both EFI (removable) and legacy BIOS
grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=schema-init --removable
grub-install --target=i386-pc /dev/sde
update-grub
CHROOT

echo "=== Installing schema-init ==="
# build static binary
cd /home/ajax80/projects/schema-init
make clean
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

echo "=== Unmounting ==="
umount "$MNT/dev/pts"
umount "$MNT/dev"
umount "$MNT/proc"
umount "$MNT/sys"
umount "$MNT/boot/efi"
umount "$MNT"

echo "=== Done — USB ready at $DEV ==="
