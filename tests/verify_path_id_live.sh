#!/bin/sh
# Live parity gate: run path_id_build over every /sys device that real udev
# assigned an ID_PATH, diff against ground truth from `udevadm info`.
# Requires: a Linux box with systemd-udev populated (blakbox). Read-only.
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/pathid_driver.c <<'EOF'
#include "path_id.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char out[PATH_ID_MAX];
    if (path_id_build("/sys", argv[1], out, sizeof out) < 0) { printf("\n"); return 0; }
    printf("%s\n", out);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/pathid_driver.c -o /tmp/pathid_driver

total=0; miss=0
for dev in $(find /sys/devices -name uevent -printf '%h\n'); do
    devpath=${dev#/sys}
    want=$(udevadm info -q property -p "$dev" 2>/dev/null | sed -n 's/^ID_PATH=//p')
    [ -z "$want" ] && continue
    total=$((total+1))
    got=$(/tmp/pathid_driver "$devpath")
    if [ "$got" != "$want" ]; then
        miss=$((miss+1))
        printf 'MISMATCH %s\n  want=%s\n  got =%s\n' "$devpath" "$want" "$got"
    fi
done
printf 'path_id live parity: %d devices, %d mismatches\n' "$total" "$miss"
[ "$miss" -eq 0 ]
