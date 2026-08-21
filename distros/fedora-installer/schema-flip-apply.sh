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

cmd="${1:-}"
case "$cmd" in
    check)
        # parity gate: verify-rules-live reads root-only /run/schema-udev data.
        # Forward its output (the wizard scrapes IN-SCOPE DIVERGENCE) and exit code.
        exec "$VERIFY"
        ;;
    arm)
        # bless THIS iso's shipped binary as the rollback baseline, back up, arm.
        ship_md5=$(cat "$SHIP_MD5_FILE" 2>/dev/null || echo "")
        SCHEMA_UDEV_GOOD_MD5="$ship_md5" "$BACKUP" backup || exit 1
        "$BACKUP" verify || exit 1
        "$ARM" arm       || exit 1
        ;;
    disarm)
        "$ARM" disarm || exit 1
        ;;
    rollback)
        "$BACKUP" rollback || exit 1
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
        echo "usage: schema-flip-apply {check|arm|disarm|rollback|is-authoritative|resolve|reboot}" >&2
        exit 2
        ;;
esac
