#!/bin/sh
# Aggregate live parity gate for builtin wiring: run run_builtins over EVERY /sys
# device, diff the union of the six builtins' owned key-subsets vs `udevadm info`,
# BOTH directions. sudo (blkid reads raw block devices). Deferred keys excluded.
#
# A device is counted whenever EITHER side emits an owned key, so under-emission
# (udev has a key we lack) is caught, not skipped. The key set is uniform across all
# devices — no per-device narrowing that could mask a delta.
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

# Keys uniquely owned by our six builtins — compared on EVERY device, both sides.
UNIFORM='^ID_PATH=|^ID_PATH_TAG=|^ID_INPUT|^ID_NET_|^ID_FS_|^ID_PART_|^ID_USB|_FROM_DATABASE='
# Keys usb_id owns but that udev ALSO emits via out-of-scope builtins (ata_id/scsi_id/
# cdrom_id) on non-USB nodes — compared ONLY on usb_device nodes, where usb_id owns them.
USBKEYS='^ID_VENDOR|^ID_MODEL|^ID_SERIAL|^ID_REVISION=|^ID_BUS=|^ID_TYPE=|^ID_INSTANCE='
# deferred keys excluded from BOTH sides (composite/informational + out-of-scope net link)
DEFER='^ID_OUI_FROM_DATABASE=|^ID_NET_DRIVER=|^ID_NET_LINK_FILE=|^ID_NET_NAME=|^ID_FS_SIZE=|^ID_FS_BLOCKSIZE=|^ID_FS_LASTBLOCK='

ours=$(mktemp); theirs=$(mktemp)
devs=0; total=0
for uev in $(find /sys/devices -name uevent -printf '%h\n'); do
    devpath=${uev#/sys}
    devname=$(sed -n 's/^DEVNAME=//p' "$uev/uevent" 2>/dev/null | head -1)
    node="-"; [ -n "$devname" ] && node="/dev/$devname"
    dt=$(sed -n 's/^DEVTYPE=//p' "$uev/uevent" 2>/dev/null | head -1)
    KEYS="$UNIFORM"; [ "$dt" = "usb_device" ] && KEYS="$UNIFORM|$USBKEYS"
    /tmp/ub_driver "$devpath" "$node" 2>/dev/null \
        | grep -E "$KEYS" | grep -Ev "$DEFER" | sort > "$ours" || true
    udevadm info -q property -p "$uev" 2>/dev/null \
        | grep -E "$KEYS" | grep -Ev "$DEFER" | sort > "$theirs" || true
    # count if EITHER side has owned keys — under-emission must not be skipped
    [ -s "$ours" ] || [ -s "$theirs" ] || continue
    devs=$((devs+1))
    if ! diff -q "$theirs" "$ours" >/dev/null; then
        echo "### MISMATCH $devpath"
        diff -u "$theirs" "$ours" | sed '1,2d' || true
        total=$((total+1))
    fi
done
echo "checked $devs devices with builtin properties; $total mismatch(es)"
rm -f "$ours" "$theirs"
[ "$total" -eq 0 ]
