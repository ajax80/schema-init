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

R="${SCHEMA_UDEV_FLIP_ROOT:-}"
STATE="$R/var/lib/schema-init/firstboot.state"
COUNT="$R/var/lib/schema-init/flip-armed-boots"
# helpers ship beside this script in both layouts (ISO /usr/local/lib/schema,
# RPM /usr/libexec/schema-init) — resolve from our own location.
LIB="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
ARM="$LIB/schema-udev-flip-arm.sh"
BACKUP="$LIB/schema-udev-flip-backup.sh"
LOG="$R/var/log/schema-init/flip-healthcheck.log"
CONSOLE="$R/dev/console"
WAITMSG="Checking devices - this can take up to 2 minutes"

[ -f "$STATE" ] && [ "$(cat "$STATE")" = "armed" ] || exit 0

log() { echo "[$(date -Is)] $*" >> "$LOG" 2>/dev/null; }

# self-heal is only possible if the flip helpers actually resolved beside us; if
# they are missing, do NOT force a reboot into a half-applied state — leave the
# box as-is and let the human recover via the recovery card / boot menu.
if [ ! -x "$ARM" ] || [ ! -x "$BACKUP" ]; then
    log "flip helpers missing under $LIB — cannot self-heal; leaving state untouched"
    exit 0
fi

notice() {
    if pgrep -x plymouthd >/dev/null 2>&1 && plymouth display-message --text="$WAITMSG" 2>/dev/null; then
        shown=plymouth
    else
        printf '%s\n' "$WAITMSG" > "$CONSOLE" 2>/dev/null
        shown=console
    fi
}

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

# wait for schema-udev to be up AND /dev to be populated before judging health.
# Neither rail orders us after udev (a dep on a crash-looping service would
# block us forever), so poll: bounded at 120s of uptime — the RTC can be hours
# off until chrony steps it. Both .svc files set start_timeout_sec=300.
shown=""
up() { cut -d. -f1 /proc/uptime; }
# the compositor opens a DRM card node; under schema-init there is no logind
# uaccess ACL, so the node must carry the 'video' (or 'render') group or the
# desktop can never take the display. A root:root 0600 card = black screen.
# Polled, not checked once: the firmware framebuffer's card0 is replaced by the
# real driver's card1 after switch-root, and the kernel creates the new node
# root:root a moment before udev sets its group -- a single look in that window
# rolled back a healthy flip (virtio-gpu VM, 2026-10-03).
dri_ok() {
    for card in /dev/dri/card[0-9]*; do
        [ -e "$card" ] || continue
        case "$(stat -c '%G' "$card" 2>/dev/null)" in video|render) return 0 ;; esac
    done
    return 1
}
deadline=$(( $(up) + 120 ))
while [ "$(up)" -lt "$deadline" ]; do
    if pgrep -x schema-udev >/dev/null 2>&1 \
       && ls /dev/input/event* >/dev/null 2>&1 \
       && dri_ok \
       && ls /dev/disk/by-uuid/* >/dev/null 2>&1; then
        break
    fi
    [ -n "$shown" ] || notice
    sleep 1
done
[ "$shown" = plymouth ] && plymouth hide-message --text="$WAITMSG" 2>/dev/null

# --- class 1: is /dev usable THIS boot? ---
pgrep -x schema-udev >/dev/null 2>&1 || rollback "schema-udev not running"
for node in /dev/null /dev/console /dev/urandom; do
    [ -e "$node" ] || rollback "missing core node $node"
done
# root disk must be addressable by uuid (fstab/boot resolve it this way)
ls /dev/disk/by-uuid/ >/dev/null 2>&1 || rollback "no /dev/disk/by-uuid entries"
# at least one input event node, or there is no keyboard/mouse
ls /dev/input/event* >/dev/null 2>&1 || rollback "no /dev/input/event* nodes"
dri_ok || rollback "no group-accessible /dev/dri card node"

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
