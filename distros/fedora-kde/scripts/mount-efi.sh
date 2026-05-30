#!/bin/sh
exec >> /var/log/mount-efi.log 2>&1
printf "mount-efi start: %s\n" "$(date)"

EFI_DEV=$(awk '$3=="vfat" && $4~/boot/ {print $1; exit}' /etc/fstab 2>/dev/null)
if [ -z "$EFI_DEV" ]; then
    for dev in /dev/nvme0n1p1 /dev/sda1 /dev/vda1; do
        blkid "$dev" 2>/dev/null | grep -qi vfat && EFI_DEV="$dev" && break
    done
fi

if [ -z "$EFI_DEV" ]; then
    printf "mount-efi: no EFI partition found\n"
    exit 0
fi

mkdir -p /boot/efi
mount -t vfat -o noatime "$EFI_DEV" /boot/efi 2>/dev/null || true

for vendor in fedora debian ubuntu arch; do
    if [ -d "/boot/efi/EFI/$vendor" ]; then
        ln -sf "/boot/efi/EFI/$vendor" /boot/grub2 2>/dev/null || true
        printf "mount-efi: linked /boot/grub2 -> /boot/efi/EFI/%s\n" "$vendor"
        break
    fi
done

printf "mount-efi done\n"
exit 0
