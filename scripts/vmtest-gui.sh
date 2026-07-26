#!/bin/bash
# vmtest-gui.sh — graphical VM rig for the VT-switch / session-handoff work.
#
# The boot-rail harness (~/schema-livetest/vmtest.sh) boots a busybox initramfs
# with no compositor, no DRM device and no D-Bus session, so it cannot test
# anything about VT switching. This rig boots a full schema-init KDE ISO with a
# real DRM device, drives VT switches over the QEMU monitor, and captures the
# guest framebuffer — so "did the screen change" is answered by an image file
# rather than by somebody's recollection.
#
#   ./vmtest-gui.sh boot <iso>       start the VM (headless-safe, GTK window)
#   ./vmtest-gui.sh key ctrl-alt-f8  inject a chord into the guest
#   ./vmtest-gui.sh shot before      capture the screen -> $RUN/before.png
#   ./vmtest-gui.sh mon "<cmd>"      raw QEMU monitor command
#   ./vmtest-gui.sh stop             shut the VM down
#
# State lives in $RUN (default /var/tmp/schema-vmgui); `boot` resets it.
set -uo pipefail

RUN=${RUN:-/var/tmp/schema-vmgui}
MON=$RUN/monitor.sock
PIDF=$RUN/qemu.pid
RAM=${RAM:-4096}
CPUS=${CPUS:-4}

mon() {
    [ -S "$MON" ] || { echo "no monitor socket at $MON — is the VM booted?" >&2; return 1; }
    printf '%s\n' "$1" | socat - "UNIX-CONNECT:$MON" 2>/dev/null
}

case "${1:-}" in
boot)
    ISO=${2:?usage: $0 boot <iso>}
    [ -f "$ISO" ] || { echo "no such ISO: $ISO" >&2; exit 1; }
    "$0" stop 2>/dev/null
    rm -rf "$RUN"; mkdir -p "$RUN"
    echo ">> booting $(basename "$ISO")  (${RAM}M, ${CPUS} cpu)"
    qemu-system-x86_64 \
        -enable-kvm -m "$RAM" -smp "$CPUS" \
        -cdrom "$ISO" -boot d \
        -device virtio-vga \
        -display gtk \
        -monitor "unix:$MON,server,nowait" \
        -serial "file:$RUN/serial.log" \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
        -pidfile "$PIDF" \
        < /dev/null > "$RUN/qemu.log" 2>&1 &
    for _ in $(seq 1 50); do [ -S "$MON" ] && break; sleep 0.2; done
    [ -S "$MON" ] || { echo "monitor socket never appeared; see $RUN/qemu.log" >&2; exit 1; }
    echo ">> up. monitor=$MON  serial=$RUN/serial.log"
    ;;
key)
    KEYS=${2:?usage: $0 key <chord>   e.g. ctrl-alt-f8}
    mon "sendkey $KEYS" >/dev/null && echo ">> sent $KEYS"
    ;;
shot)
    NAME=${2:-shot}
    mon "screendump $RUN/$NAME.ppm" >/dev/null
    for _ in $(seq 1 25); do [ -s "$RUN/$NAME.ppm" ] && break; sleep 0.2; done
    if [ -s "$RUN/$NAME.ppm" ]; then
        magick "$RUN/$NAME.ppm" "$RUN/$NAME.png" 2>/dev/null && rm -f "$RUN/$NAME.ppm"
        echo ">> $RUN/$NAME.png"
    else
        echo "screendump produced nothing" >&2; exit 1
    fi
    ;;
mon)
    mon "${2:?usage: $0 mon \"<qemu monitor command>\"}"
    ;;
stop)
    [ -S "$MON" ] && mon quit >/dev/null 2>&1
    [ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null
    sleep 0.5
    echo ">> stopped"
    ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
