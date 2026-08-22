#!/bin/bash
# schema-flip-apply — the PRIVILEGED half of the first-boot schema-udev flip.
#
# The GUI wizard (schema-firstboot-wizard) runs as the unprivileged desktop
# user so yad can draw in their Wayland session. Everything that needs root —
# reading the shadow rule-data, backing up, arming the LIVE flag, rolling back,
# removing the system autostart, rebooting — is funneled through THIS script,
# which the wizard invokes as `sudo /usr/local/lib/schema/schema-flip-apply <cmd>`.
#
# A passwordless sudoers.d rule (installed by schema.ks for the login user)
# permits exactly this one script — so this file is the whole security surface.
# Keep it tight: fixed absolute paths, a closed set of subcommands, no eval of
# caller-supplied strings.
#
# There is no polkit auth agent under schema-init (the KDE agent is a systemd
# *user* unit, which cannot run here), so pkexec is not an option — hence sudo.
set -u

LIB=/usr/local/lib/schema
ARM="$LIB/schema-udev-flip-arm.sh"
BACKUP="$LIB/schema-udev-flip-backup.sh"
VERIFY="$LIB/verify-rules-live"
SHIP_MD5_FILE=/etc/schema-init/schema-udev.ship-md5
SYS_AUTOSTART=/etc/xdg/autostart/schema-firstboot.desktop
# ROOT state the headless seatbelt reads (distinct from the wizard's own
# user-tree state). The helper is the ONLY writer — it is the root boundary.
ROOT_STATE=/var/lib/schema-init/firstboot.state
ARMED_BOOTS=/var/lib/schema-init/flip-armed-boots

cmd="${1:-}"
case "$cmd" in
    check)
        # eligibility gate, PERMISSIVE model: exit 0 unless a HARMFUL divergence
        # remains (a missing boot/fstab exact-path link, or a missing tag). Extra
        # symlinks and reachable-by-sibling misses are harmless supersets — the
        # boot healthcheck backstops anything that actually breaks. Forward the
        # full output (the wizard scrapes HARMFUL) and exit code.
        exec "$VERIFY" --permissive
        ;;
    report)
        # Capture the full flip picture to a local, world-readable report so an
        # ineligible machine can show its owner exactly WHY (and hand it to
        # someone for help). No network, no export — purely local (0644).
        R=/var/log/schema-flip-report.txt
        {
            echo "=== schema-udev flip report ==="
            echo "date:     $(date -Is)"
            echo "host:     $(uname -n)   kernel: $(uname -r)"
            echo "cmdline:  $(cat /proc/cmdline)"
            echo "schema-udev running: $(pgrep -x schema-udev >/dev/null && echo yes || echo no)"
            echo "arm status: $("$ARM" status 2>&1 | head -1)"
            echo
            echo "=== divergence detail (strict: every tier) ==="
            "$VERIFY" 2>&1
            echo
            echo "=== block-device by-* links (fstab/boot resolve these) ==="
            ls -l /dev/disk/by-uuid /dev/disk/by-partuuid /dev/disk/by-label 2>/dev/null
            echo
            echo "=== input devices ==="
            ls -l /dev/input/by-path 2>/dev/null
            echo
            echo "=== fstab ==="
            grep -vE '^\s*#|^\s*$' /etc/fstab 2>/dev/null
            echo
            echo "=== recent kernel messages (udev/drm) ==="
            dmesg 2>/dev/null | grep -iE 'udev|drm|i915|input|error|fail' | tail -60
        } > "$R" 2>&1
        chmod 0644 "$R"
        echo "$R"
        ;;
    arm)
        # bless THIS iso's shipped binary as the rollback baseline, back up, arm.
        ship_md5=$(cat "$SHIP_MD5_FILE" 2>/dev/null || echo "")
        SCHEMA_UDEV_GOOD_MD5="$ship_md5" "$BACKUP" backup || exit 1
        "$BACKUP" verify || exit 1
        "$ARM" arm       || exit 1
        # tell the seatbelt a flip is pending verification, fresh boot counter.
        install -d "$(dirname "$ROOT_STATE")"
        echo armed > "$ROOT_STATE"
        rm -f "$ARMED_BOOTS"
        ;;
    confirm)
        # the GUI saw the flipped desktop come up healthy -> stop the seatbelt.
        install -d "$(dirname "$ROOT_STATE")"
        echo done > "$ROOT_STATE"
        rm -f "$ARMED_BOOTS"
        ;;
    disarm)
        "$ARM" disarm || exit 1
        ;;
    rollback)
        "$BACKUP" rollback || exit 1
        # back to safe state; the seatbelt must not act again.
        install -d "$(dirname "$ROOT_STATE")"
        echo skipped > "$ROOT_STATE"
        rm -f "$ARMED_BOOTS"
        ;;
    is-authoritative)
        # is schema-udev actually the authority right now?
        pgrep -x schema-udev >/dev/null 2>&1 || exit 1
        "$ARM" status | grep -q '^ARMED'     || exit 1
        udevadm info /dev/null >/dev/null 2>&1 || exit 1
        ;;
    resolve)
        # remove the system autostart so the wizard never nags again (the user's
        # own ~/Desktop launcher is removed by the wizard itself, as the user).
        rm -f "$SYS_AUTOSTART"
        ;;
    reboot)
        systemctl reboot 2>/dev/null || schema-ctl reboot 2>/dev/null || reboot
        ;;
    *)
        echo "usage: schema-flip-apply {check|report|arm|confirm|disarm|rollback|is-authoritative|resolve|reboot}" >&2
        exit 2
        ;;
esac
