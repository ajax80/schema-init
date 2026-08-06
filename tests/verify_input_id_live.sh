#!/bin/sh
# Live parity gate: run input_id_build over every /sys device real udev gave
# ID_INPUT=1, diff emitted keys vs `udevadm info` BOTH directions.
# Read-only. Requires a systemd-udev-populated box (blakbox).
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/inputid_driver.c <<'EOF'
#include "input_id.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct uevent ev;
    if (input_id_build("/sys", argv[1], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/inputid_driver.c -o /tmp/inputid_driver

props=$(mktemp)
misses=$(mktemp)
total=0
for dev in $(find /sys/devices -name uevent -printf '%h\n'); do
    devpath=${dev#/sys}
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -qx 'ID_INPUT=1' "$props" || continue
    total=$((total + 1))
    emitted=$(/tmp/inputid_driver "$devpath")
    # forward: every key we emit must be present verbatim in udev's set
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$props" || printf 'MISMATCH(val) %s | emit=%s\n' "$devpath" "$line"
    done >> "$misses"
    # reverse: every ID_INPUT* udev has, we must emit (under-emission)
    grep -oE '^ID_INPUT[A-Z_]*=1' "$props" | while IFS= read -r uline; do
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$devpath" "$uline"
    done >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'input_id live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$misses"
[ "$miss" -eq 0 ]
