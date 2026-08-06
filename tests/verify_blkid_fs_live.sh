#!/bin/sh
# Live parity gate: run blkid_fs_build over every /sys/class/block node, diff the
# identity ID_FS_* subset vs `udevadm info` BOTH directions. SIZE/BLOCKSIZE/LASTBLOCK
# are deferred (excluded both sides). Reads raw devices -> runs under sudo.
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/blkidfs_driver.c <<'EOF'
#include "blkid_fs.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;                 /* argv[1]=devpath argv[2]=devnode */
    struct uevent ev;
    if (blkid_fs_build("/sys", argv[1], argv[2], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/blkidfs_driver.c -o /tmp/blkidfs_driver

# identity fields only; exclude the deferred size trio
KEYS='^ID_FS_(TYPE|USAGE|UUID|UUID_ENC|LABEL|LABEL_ENC|UUID_SUB|UUID_SUB_ENC|VERSION)='
DEFER='^ID_FS_(SIZE|BLOCKSIZE|LASTBLOCK)='

props=$(mktemp)
fsprops=$(mktemp)
misses=$(mktemp)
total=0
for blk in /sys/class/block/*; do
    [ -e "$blk" ] || continue
    name=$(basename "$blk")
    dev=$(readlink -f "$blk"); devpath=${dev#/sys}
    devnode="/dev/$name"
    [ -b "$devnode" ] || continue
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -E "$KEYS" "$props" | grep -vE "$DEFER" > "$fsprops" || true
    emitted=$(sudo /tmp/blkidfs_driver "$devpath" "$devnode")
    [ -n "$emitted" ] || [ -s "$fsprops" ] || continue
    total=$((total + 1))
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$fsprops" || printf 'MISMATCH(val) %s | emit=%s\n' "$name" "$line"
    done >> "$misses"
    while IFS= read -r uline; do
        [ -n "$uline" ] || continue
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$name" "$uline"
    done < "$fsprops" >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'blkid_fs live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$fsprops" "$misses"
[ "$miss" -eq 0 ]
