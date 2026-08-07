#!/bin/sh
# Aggregate live parity gate for builtin wiring: run run_builtins over EVERY /sys
# device, diff the union of the six builtins' owned key-subsets vs `udevadm info`,
# BOTH directions. sudo (blkid reads raw block devices). Deferred keys excluded.
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/ub_driver.c <<'EOF'
#include "udev_builtins.h"
#include <stdio.h>
#include <string.h>
/* argv[1]=devpath (under /sys), argv[2]=devnode or "-" */
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *devnode = strcmp(argv[2], "-") ? argv[2] : NULL;
    char sysdir[PATH_MAX];
    snprintf(sysdir, sizeof sysdir, "/sys%s", argv[1]);
    struct uevent ev; ev.n = 0;
    uevent_from_sysfs("/sys", sysdir, &ev);
    int base = ev.n;
    run_builtins("/sys", argv[1], devnode, &ev);
    for (int i = base; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/ub_driver.c -o /tmp/ub_driver

# owned-subset key prefixes across the six builtins
KEYS='^ID_PATH=|^ID_PATH_TAG=|^ID_VENDOR|^ID_MODEL|^ID_SERIAL|^ID_REVISION=|^ID_TYPE=|^ID_USB|^ID_BUS=|^ID_INSTANCE=|^ID_PCI_|^ID_INPUT|^ID_NET_|^ID_FS_|^ID_PART_|_FROM_DATABASE='
# deferred keys excluded from BOTH sides
DEFER='^ID_OUI_FROM_DATABASE=|^ID_NET_DRIVER=|^ID_FS_SIZE=|^ID_FS_BLOCKSIZE=|^ID_FS_LASTBLOCK='

ours=$(mktemp); theirs=$(mktemp); total=0; devs=0
for uev in $(find /sys/devices -name uevent -printf '%h\n'); do
    devpath=${uev#/sys}
    devname=$(sed -n 's/^DEVNAME=//p' "$uev/uevent" 2>/dev/null | head -1)
    node="-"; [ -n "$devname" ] && node="/dev/$devname"
    sub=$(sed -n 's/^SUBSYSTEM=//p' "$uev/uevent" 2>/dev/null | head -1)
    dt=$(sed -n 's/^DEVTYPE=//p' "$uev/uevent" 2>/dev/null | head -1)
    has_ma=$(grep -c '^MODALIAS=' "$uev/uevent" 2>/dev/null || true)
    if [ "$sub" = "usb" ] && [ "$dt" = "usb_device" ]; then
        kpat='^ID_PATH=|^ID_PATH_TAG=|^ID_VENDOR|^ID_MODEL|^ID_SERIAL|^ID_REVISION=|^ID_TYPE=|^ID_USB|^ID_BUS=|^ID_INSTANCE=|^ID_PCI_|_FROM_DATABASE='
    elif [ "$has_ma" -gt 0 ]; then
        kpat='^ID_PATH=|^ID_PATH_TAG=|^ID_INPUT|^ID_NET_|^ID_FS_|^ID_PART_|_FROM_DATABASE='
    else
        kpat='^ID_PATH=|^ID_PATH_TAG=|^ID_INPUT|^ID_NET_|^ID_FS_|^ID_PART_'
    fi
    sudo /tmp/ub_driver "$devpath" "$node" 2>/dev/null \
        | grep -E "$kpat" | grep -Ev "$DEFER" | sort > "$ours" || true
    udevadm info -q property -p "$uev" 2>/dev/null \
        | grep -E "$kpat" | grep -Ev "$DEFER" | sort > "$theirs" || true
    [ -s "$ours" ] || continue
    devs=$((devs+1))
    if ! diff -u "$theirs" "$ours" >/dev/null; then
        echo "### MISMATCH $devpath"; diff -u "$theirs" "$ours" || true
        total=$((total+1))
    fi
done
echo "checked $devs devices with builtin properties; $total mismatch(es)"
rm -f "$ours" "$theirs"
[ "$total" -eq 0 ]
