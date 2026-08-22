#!/bin/sh
# Headless seatbelt for the schema-udev flip. Runs as a schema-init oneshot on
# EVERY boot, before the desktop — so it can undo a flip that broke the machine
# even when there is no working GUI to run the wizard. This is what makes the
# PERMISSIVE flip safe to hand to a novice: the gate lets a flip through as long
# as nothing HARMFUL diverges, and this check is the backstop that auto-heals if
# the box actually comes up unusable.
#
# Two failure classes it catches:
#   1. Unusable /dev THIS boot (schema-udev dead, no input, DRM node the
#      compositor can't open) -> roll back immediately.
#   2. Desktop never comes up at all. The GUI wizard confirms success from
#      inside the session, so if it never confirms across a FULL armed boot,
#      the desktop isn't coming up -> roll back on the next boot. A boot counter
#      gives this without a fragile in-boot timer.
set -u

STATE=/var/lib/schema-init/firstboot.state
COUNT=/var/lib/schema-init/flip-armed-boots
LIB=/usr/local/lib/schema
ARM="$LIB/schema-udev-flip-arm.sh"
BACKUP="$LIB/schema-udev-flip-backup.sh"
LOG=/var/log/schema-init/flip-healthcheck.log

[ -f "$STATE" ] && [ "$(cat "$STATE")" = "armed" ] || exit 0

log() { echo "[$(date -Is)] $*" >> "$LOG" 2>/dev/null; }

rollback() {
    log "flip UNHEALTHY ($1) — rolling back to systemd-udev"
    "$ARM" disarm || true
    SCHEMA_UDEV_SKIP_ROOT=0 "$BACKUP" rollback >> "$LOG" 2>&1 || log "rollback reported error"
    echo skipped > "$STATE"
    rm -f "$COUNT"
    log "rolled back; rebooting into safe state"
    schema-ctl reboot 2>/dev/null || reboot -f
    exit 0
}

# give schema-udev a moment to populate /dev after the flip
i=0; while [ $i -lt 10 ]; do pgrep -x schema-udev >/dev/null 2>&1 && break; i=$((i+1)); sleep 1; done

# --- class 1: is /dev usable THIS boot? ---
pgrep -x schema-udev >/dev/null 2>&1 || rollback "schema-udev not running"
for node in /dev/null /dev/console /dev/urandom; do
    [ -e "$node" ] || rollback "missing core node $node"
done
# root disk must be addressable by uuid (fstab/boot resolve it this way)
ls /dev/disk/by-uuid/ >/dev/null 2>&1 || rollback "no /dev/disk/by-uuid entries"
# at least one input event node, or there is no keyboard/mouse
ls /dev/input/event* >/dev/null 2>&1 || rollback "no /dev/input/event* nodes"
# the compositor opens a DRM card node; under schema-init there is no logind
# uaccess ACL, so the node must carry the 'video' (or 'render') group or the
# desktop can never take the display. A root:root 0600 card = black screen.
dri_ok=0
for card in /dev/dri/card[0-9]*; do
    [ -e "$card" ] || continue
    grp=$(stat -c '%G' "$card" 2>/dev/null)
    case "$grp" in video|render) dri_ok=1 ;; esac
done
[ "$dri_ok" = 1 ] || rollback "no group-accessible /dev/dri card node"

# --- class 2: did the desktop ever confirm? ---
# healthy /dev this boot. Bump the armed-boot counter. If we have already been
# through a full armed boot without the GUI flipping state to 'done', the desktop
# is not coming up -> heal.
n=$(cat "$COUNT" 2>/dev/null || echo 0)
case "$n" in ''|*[!0-9]*) n=0 ;; esac
n=$((n + 1))
echo "$n" > "$COUNT"
if [ "$n" -ge 2 ]; then
    rollback "desktop never confirmed across $n armed boots"
fi

log "flip healthy (armed boot $n) — leaving armed; GUI will confirm at login"
exit 0
