#!/bin/sh
# Live parity gate: run usb_id_build over every /sys device real udev gave a
# USB identity (ID_USB_VENDOR_ID), diff emitted keys vs `udevadm info`.
# Read-only. Requires a systemd-udev-populated box (blakbox).
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/usbid_driver.c <<'EOF'
#include "usb_id.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct uevent ev;
    if (usb_id_build("/sys", argv[1], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/usbid_driver.c -o /tmp/usbid_driver

props=$(mktemp)
misses=$(mktemp)
total=0
for dev in $(find /sys/devices -name uevent -printf '%h\n'); do
    devpath=${dev#/sys}
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -q '^ID_USB_VENDOR_ID=' "$props" || continue
    total=$((total + 1))
    /tmp/usbid_driver "$devpath" | while IFS= read -r line; do
        key=${line%%=*}
        # unprefixed ID_TYPE is overwritten by scsi/ata on block nodes; ID_USB_TYPE is authoritative
        [ "$key" = "ID_TYPE" ] && continue
        grep -qxF "$line" "$props" || {
            udv=$(grep "^$key=" "$props" || echo "(absent)")
            printf 'MISMATCH %s\n  emit=%s\n  udev=%s\n' "$devpath" "$line" "$udv"
        }
    done >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'usb_id live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$misses"
[ "$miss" -eq 0 ]
