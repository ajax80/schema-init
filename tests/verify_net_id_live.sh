#!/bin/sh
# Live parity gate: run net_id_build over every /sys/class/net device, diff the
# net_id-owned keys vs `udevadm info` BOTH directions.
# Read-only. Requires a systemd-udev-populated box (blakbox).
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/netid_driver.c <<'EOF'
#include "net_id.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct uevent ev;
    if (net_id_build("/sys", argv[1], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/netid_driver.c -o /tmp/netid_driver

# net_id owns exactly these keys; everything else in udev's set is out of scope.
KEYS='^(ID_NET_NAMING_SCHEME|ID_NET_NAME_MAC|ID_NET_NAME_PATH|ID_NET_NAME_SLOT|ID_NET_NAME_ONBOARD|ID_NET_LABEL_ONBOARD)='

props=$(mktemp)
netprops=$(mktemp)
misses=$(mktemp)
total=0
for ifp in /sys/class/net/*; do
    [ -e "$ifp" ] || continue
    dev=$(readlink -f "$ifp")             # /sys/devices/.../net/<if>
    devpath=${dev#/sys}
    total=$((total + 1))
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -E "$KEYS" "$props" > "$netprops" || true
    emitted=$(/tmp/netid_driver "$devpath")
    # forward: every key we emit must be present verbatim in udev's net_id subset
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$netprops" || printf 'MISMATCH(val) %s | emit=%s\n' "$devpath" "$line"
    done >> "$misses"
    # reverse: every net_id key udev has, we must emit (under-emission)
    while IFS= read -r uline; do
        [ -n "$uline" ] || continue
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$devpath" "$uline"
    done < "$netprops" >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'net_id live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$netprops" "$misses"
[ "$miss" -eq 0 ]
