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
