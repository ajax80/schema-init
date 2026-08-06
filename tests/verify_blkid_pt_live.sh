#!/bin/sh
# Live parity gate: run blkid_pt_build over every /sys/class/block node, diff the
# ID_PART_TABLE_* / ID_PART_ENTRY_* subset vs `udevadm info` BOTH directions.
# Reads raw block devices -> runs the driver under sudo (user not in 'disk' group).
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/blkidpt_driver.c <<'EOF'
#include "blkid_pt.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;                 /* argv[1]=devpath argv[2]=devnode */
    struct uevent ev;
    if (blkid_pt_build("/sys", argv[1], argv[2], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/blkidpt_driver.c -o /tmp/blkidpt_driver

KEYS='^(ID_PART_TABLE_TYPE|ID_PART_TABLE_UUID|ID_PART_ENTRY_[A-Z_]*)='

props=$(mktemp)
ptprops=$(mktemp)
misses=$(mktemp)
total=0
for blk in /sys/class/block/*; do
    [ -e "$blk" ] || continue
    name=$(basename "$blk")
    dev=$(readlink -f "$blk"); devpath=${dev#/sys}
    devnode="/dev/$name"
    [ -b "$devnode" ] || continue
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -E "$KEYS" "$props" > "$ptprops" || true
    emitted=$(sudo /tmp/blkidpt_driver "$devpath" "$devnode")
    # skip nodes with no partition table on either side (e.g. zram0)
    [ -n "$emitted" ] || [ -s "$ptprops" ] || continue
    total=$((total + 1))
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$ptprops" || printf 'MISMATCH(val) %s | emit=%s\n' "$name" "$line"
    done >> "$misses"
    while IFS= read -r uline; do
        [ -n "$uline" ] || continue
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$name" "$uline"
    done < "$ptprops" >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'blkid_pt live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$ptprops" "$misses"
[ "$miss" -eq 0 ]
