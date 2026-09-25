#!/bin/sh
# The ONLY blessed way to arm/disarm the schema-udev LIVE flag.
# Ad-hoc `touch` could silently fail or hit the wrong path; this asserts.
set -eu

FLAG=/etc/schema-init/schema-udev.live   # MUST match SCHEMA_UDEV_LIVE_FLAG in schema-udev.c

case "${1:-status}" in
arm)
    [ "$(id -u)" -eq 0 ] || { echo "must be root" >&2; exit 2; }
    : > "$FLAG"
    if [ ! -e "$FLAG" ]; then
        echo "ARM FAILED: $FLAG absent after write — DO NOT REBOOT" >&2
        exit 1
    fi
    ls -la "$FLAG"
    # schema-udev keeps kernel NIC names (wlan0/eth0); systemd-udev renames
    # them (wlp0s29u1u1). An NM profile pinned to the old name never
    # autoconnects after the flip, so unpin profiles bound to physical NICs.
    if command -v nmcli >/dev/null 2>&1; then
        nmcli -t -f UUID connection show 2>/dev/null | while read -r uuid; do
            ifn=$(nmcli -g connection.interface-name connection show "$uuid" 2>/dev/null)
            [ -n "$ifn" ] && [ -e "/sys/class/net/$ifn/device" ] || continue
            nmcli connection modify "$uuid" connection.interface-name "" \
                && echo "unpinned NM profile $uuid from $ifn"
        done
    fi
    echo "ARMED OK — $FLAG present (daemon access() will return 0 -> LIVE)"
    ;;
disarm)
    [ "$(id -u)" -eq 0 ] || { echo "must be root" >&2; exit 2; }
    rm -f "$FLAG"
    [ -e "$FLAG" ] && { echo "DISARM FAILED: $FLAG still present" >&2; exit 1; }
    echo "DISARMED OK — $FLAG gone (daemon -> dry-run)"
    ;;
status)
    if [ -e "$FLAG" ]; then echo "ARMED   ($FLAG present)"; else echo "disarmed ($FLAG absent)"; fi
    ;;
*)
    echo "usage: $0 {arm|disarm|status}" >&2; exit 2 ;;
esac
