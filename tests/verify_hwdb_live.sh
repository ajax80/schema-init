#!/bin/sh
# Live parity gate: run hwdb_build over every /sys device with a modalias, diff the
# *_FROM_DATABASE subset vs `udevadm info` BOTH directions. ID_OUI_FROM_DATABASE is a
# composite OUI lookup (deferred) and is excluded. hwdb.bin is world-readable -> no sudo.
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/hwdb_driver.c <<'EOF'
#include "hwdb.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct uevent ev;
    if (hwdb_build("/sys", argv[1], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/hwdb_driver.c -o /tmp/hwdb_driver

KEYS='_FROM_DATABASE='
DEFER='^ID_OUI_FROM_DATABASE='

props=$(mktemp)
dbprops=$(mktemp)
misses=$(mktemp)
total=0
for dev in $(find /sys/devices -name modalias -printf '%h\n'); do
    devpath=${dev#/sys}
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -E "$KEYS" "$props" | grep -vE "$DEFER" > "$dbprops" || true
    emitted=$(/tmp/hwdb_driver "$devpath" | grep -E "$KEYS" | grep -vE "$DEFER" || true)
    [ -n "$emitted" ] || continue
    total=$((total + 1))
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$dbprops" || printf 'MISMATCH(val) %s | emit=%s\n' "$devpath" "$line"
    done >> "$misses"
    while IFS= read -r uline; do
        [ -n "$uline" ] || continue
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$devpath" "$uline"
    done < "$dbprops" >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'hwdb live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$dbprops" "$misses"
[ "$miss" -eq 0 ]
