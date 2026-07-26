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
    # A window is nice but optional — screendump captures the framebuffer either
    # way, and qemu-system-x86-core ships without any UI backend.
    DISP=${DISPLAY_BACKEND:-}
    if [ -z "$DISP" ]; then
        if qemu-system-x86_64 -display help 2>/dev/null | grep -qx gtk && [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
            DISP=gtk
        else
            DISP=none
        fi
    fi
    # Direct kernel boot (default): pull vmlinuz/initrd straight out of the ISO
    # and set our own cmdline. The ISO's grub.cfg uses `quiet` with no console=,
    # so a bootloader boot goes silent right after handoff and there is no way
    # to see what the kernel is doing. Booting -kernel also skips the bootloader
    # entirely, which is faster and sidesteps the ISO's 4-sector BIOS El Torito
    # image (which does not boot at all). BOOTLOADER=1 to use the ISO's own path.
    KOPT=()
    if [ "${BOOTLOADER:-0}" != "1" ]; then
        mkdir -p "$RUN/boot"
        LABEL=$(blkid -o value -s LABEL "$ISO" 2>/dev/null || echo SCHEMA_FEDORA)
        if ! xorriso -osirrox on -indev "$ISO" \
                -extract /images/pxeboot/vmlinuz "$RUN/boot/vmlinuz" \
                -extract /images/pxeboot/initrd.img "$RUN/boot/initrd.img" \
                >/dev/null 2>&1; then
            echo "could not extract kernel/initrd from $ISO" >&2; exit 1
        fi
        # The ISO's own grub.cfg never sets init=, so a normal boot of this image
        # runs systemd, not schema-init. INIT= (default /sbin/schema-init) is
        # what actually puts schema-init on PID 1; INIT=systemd for a control run.
        INIT=${INIT:-/sbin/schema-init}
        APPEND="root=live:CDLABEL=$LABEL rd.live.image rd.live.squashimg=filesystem.squashfs rw console=tty0 console=ttyS0,115200"
        [ "$INIT" = "systemd" ] || APPEND="$APPEND init=$INIT"
        echo ">> init=$INIT"
        KOPT=(-kernel "$RUN/boot/vmlinuz" -initrd "$RUN/boot/initrd.img" -append "$APPEND")
        echo ">> direct kernel boot, label=$LABEL"
    fi

    # UEFI=1 boots via OVMF. The Fedora ISO builder's BIOS El Torito image is
    # only 4 sectors and does not boot; its UEFI image is the working one.
    FW=()
    if [ "${UEFI:-0}" = "1" ]; then
        OVMF_CODE=${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}
        OVMF_VARS=${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}
        [ -f "$OVMF_CODE" ] || { echo "no OVMF firmware at $OVMF_CODE" >&2; exit 1; }
        cp "$OVMF_VARS" "$RUN/vars.fd"
        FW=(-drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE"
            -drive "if=pflash,format=raw,unit=1,file=$RUN/vars.fd")
        echo ">> firmware: UEFI (OVMF)"
    fi
    echo ">> booting $(basename "$ISO")  (${RAM}M, ${CPUS} cpu, display=$DISP)"
    qemu-system-x86_64 \
        -enable-kvm -m "$RAM" -smp "$CPUS" \
        "${FW[@]}" "${KOPT[@]}" \
        -cdrom "$ISO" -boot d \
        -device virtio-vga \
        -display "$DISP" \
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
type)
    # Type a line into the guest, one sendkey per character. The ISO ships
    # getty-tty2 as `agetty --autologin root`, so ctrl-alt-f2 gives a root shell
    # with no schema-ctl in the image -- reading the per-service logs from that
    # shell is the only way to see why a service did not come up.
    TEXT=${2?usage: $0 type \"<line>\"}
    declare -A K=(
        [' ']=spc ['/']=slash ['-']=minus ['.']=dot [',']=comma [';']=semicolon
        ["'"]=apostrophe ['=']=equal ['[']=bracket_left [']']=bracket_right
        ['\']=backslash ['`']=grave_accent
        ['_']=shift-minus [':']=shift-semicolon ['|']=shift-backslash
        ['~']=shift-grave_accent ['?']=shift-slash ['"']=shift-apostrophe
        ['<']=shift-comma ['>']=shift-dot ['+']=shift-equal ['*']=shift-8
        ['(']=shift-9 [')']=shift-0 ['{']=shift-bracket_left ['}']=shift-bracket_right
        ['!']=shift-1 ['@']=shift-2 ['#']=shift-3 ['$']=shift-4 ['%']=shift-5
        ['^']=shift-6 ['&']=shift-7
    )
    for (( i=0; i<${#TEXT}; i++ )); do
        c=${TEXT:i:1}
        if [[ -n ${K[$c]:-} ]]; then k=${K[$c]}
        elif [[ $c =~ [a-z0-9] ]]; then k=$c
        elif [[ $c =~ [A-Z] ]]; then k="shift-${c,}"
        else echo "  (skipping unmappable char '$c')" >&2; continue
        fi
        mon "sendkey $k" >/dev/null
    done
    mon "sendkey ret" >/dev/null
    echo ">> typed: $TEXT"
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
