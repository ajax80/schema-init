#!/bin/sh
# Aggregate live parity gate for builtin wiring (sub-project A = IMPORT{builtin}).
#
# A wires the six builtins so run_builtins() dispatches the right ones per device and
# merges their output. This gate proves that, per builtin, on the device class that
# builtin is the ORIGIN for, run_builtins() reproduces the builtin's correct output.
#
# It deliberately does NOT test IMPORT{parent} propagation to child devices or
# constructed/composite hwdb keys (usb idVendor/idProduct, evdev/OUI/...): those are
# the rules engine = sub-project B. Accordingly:
#   - each builtin's keys are compared only on that builtin's origin device class;
#   - the hwdb oracle is `systemd-hwdb query <modalias>` (the pure IMPORT{builtin}
#     result for the device's OWN modalias), not `udevadm info` (which also shows
#     parent-inherited *_FROM_DATABASE);
#   - usb_id keys are compared only on usb_device nodes (where usb_id owns them;
#     elsewhere ata_id/scsi_id/cdrom_id — out of scope — emit the same prefixes).
# sudo: blkid reads raw block devices.
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/ub_driver.c <<'EOF'
#include "udev_builtins.h"
#include <stdio.h>
#include <string.h>
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

PATH_K='^ID_PATH=|^ID_PATH_TAG='
BLKID_K='^ID_FS_|^ID_PART_'
USB_K='^ID_USB|^ID_VENDOR|^ID_MODEL|^ID_SERIAL|^ID_REVISION=|^ID_BUS=|^ID_TYPE=|^ID_INSTANCE='
INPUT_K='^ID_INPUT'
NET_K='^ID_NET_'
HWDB_K='_FROM_DATABASE='
DEFER_FS='^ID_FS_SIZE=|^ID_FS_BLOCKSIZE=|^ID_FS_LASTBLOCK='
DEFER_NET='^ID_NET_DRIVER=|^ID_NET_LINK_FILE=|^ID_NET_NAME='
DEFER_HWDB='^ID_OUI_FROM_DATABASE='
# subsystems udev runs path_id on (rest inherit ID_PATH via the rules engine = B)
PATH_SUBS=" pci usb platform block input hidraw rfkill "

ours=$(mktemp); theirs=$(mktemp); allo=$(mktemp)
total=0; devs=0

check() {   # $1=label $2=keypat $3=defer(may be empty); compares $ours vs $theirs
    devs=$((devs+1))
    diff -q "$theirs" "$ours" >/dev/null && return 0
    echo "### $1 MISMATCH $dp"; diff "$theirs" "$ours" | grep -E '^[<>]' || true
    total=$((total+1))
}

for uev in $(find /sys/devices -name uevent -printf '%h\n'); do
    dp=${uev#/sys}
    devname=$(sed -n 's/^DEVNAME=//p' "$uev/uevent" 2>/dev/null | head -1)
    node="-"; [ -n "$devname" ] && node="/dev/$devname"
    sub=$(basename "$(readlink "$uev/subsystem" 2>/dev/null)" 2>/dev/null || true)
    dt=$(sed -n 's/^DEVTYPE=//p' "$uev/uevent" 2>/dev/null | head -1)
    kname=${dp##*/}
    sudo /tmp/ub_driver "$dp" "$node" 2>/dev/null > "$allo" || true

    # path_id — only on the subsystems udev path_id's, virtual block excluded
    case "$PATH_SUBS" in *" $sub "*)
        if ! { [ "$sub" = block ] && case "$dp" in */virtual/*) true;; *) false;; esac; }; then
            grep -E "$PATH_K" "$allo" | sort > "$ours" || true
            udevadm info -q property -p "$uev" 2>/dev/null | grep -E "$PATH_K" | sort > "$theirs" || true
            { [ -s "$ours" ] || [ -s "$theirs" ]; } && check path_id
        fi ;;
    esac

    # blkid — block disk/partition, not optical/mmc-boot
    if [ "$sub" = block ] && { [ "$dt" = disk ] || [ "$dt" = partition ]; } \
       && ! echo "$kname" | grep -qE '^sr|^mmcblk.*boot'; then
        grep -E "$BLKID_K" "$allo" | grep -Ev "$DEFER_FS" | sort > "$ours" || true
        udevadm info -q property -p "$uev" 2>/dev/null | grep -E "$BLKID_K" | grep -Ev "$DEFER_FS" | sort > "$theirs" || true
        { [ -s "$ours" ] || [ -s "$theirs" ]; } && check blkid
    fi

    # usb_id — usb_device nodes only. Exclude *_FROM_DATABASE: those are hwdb's
    # CONSTRUCTED usb:vVVVVpPPPP lookup (from idVendor/idProduct) = deferred composite
    # key, not usb_id's (usb_id emits ID_USB_VENDOR/ID_VENDOR from string descriptors).
    if [ "$dt" = usb_device ]; then
        grep -E "$USB_K" "$allo" | grep -Ev "$HWDB_K" | sort > "$ours" || true
        udevadm info -q property -p "$uev" 2>/dev/null | grep -E "$USB_K" | grep -Ev "$HWDB_K" | sort > "$theirs" || true
        { [ -s "$ours" ] || [ -s "$theirs" ]; } && check usb_id
    fi

    # input_id — input subsystem
    if [ "$sub" = input ]; then
        grep -E "$INPUT_K" "$allo" | sort > "$ours" || true
        udevadm info -q property -p "$uev" 2>/dev/null | grep -E "$INPUT_K" | sort > "$theirs" || true
        { [ -s "$ours" ] || [ -s "$theirs" ]; } && check input_id
    fi

    # net_id — net subsystem
    if [ "$sub" = net ]; then
        grep -E "$NET_K" "$allo" | grep -Ev "$DEFER_NET" | sort > "$ours" || true
        udevadm info -q property -p "$uev" 2>/dev/null | grep -E "$NET_K" | grep -Ev "$DEFER_NET" | sort > "$theirs" || true
        { [ -s "$ours" ] || [ -s "$theirs" ]; } && check net_id
    fi

    # hwdb — oracle is `systemd-hwdb query <own modalias>` (pure IMPORT{builtin}),
    # NOT udevadm (which adds parent-inherited *_FROM_DATABASE = sub-project B).
    ma=$(cat "$uev/modalias" 2>/dev/null || true)
    if [ -n "$ma" ]; then
        grep -E "$HWDB_K" "$allo" | grep -Ev "$DEFER_HWDB" | sort > "$ours" || true
        systemd-hwdb query "$ma" 2>/dev/null | grep -E "$HWDB_K" | grep -Ev "$DEFER_HWDB" | sort > "$theirs" || true
        { [ -s "$ours" ] || [ -s "$theirs" ]; } && check hwdb
    fi
done
echo "checked $devs builtin-origin comparisons; $total mismatch(es)"
rm -f "$ours" "$theirs" "$allo"
[ "$total" -eq 0 ]
