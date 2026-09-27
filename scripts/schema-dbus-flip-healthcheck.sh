#!/bin/sh
# Headless seatbelt for the schema-dbus flip: a schema-init oneshot on every
# boot, with NO dep= at all. A dead system bus is a black screen with no GUI
# to recover from, so this is what undoes a bad flip unattended.
#
#   1. THIS boot: the broker (not the stock fallback) owns the system bus,
#      answers GetId, and login1 / NetworkManager / PolicyKit1 each appear on
#      it within 120s (only those whose .svc is on this box). Else roll back.
#   2. The desktop never confirmed across a full armed boot -> roll back.
#
# Deliberately no dep= on anything, dbus included: a service that keeps dying
# goes DORMANT, not EXCISED, and a DORMANT dep blocks its dependents forever.
# With dep=dbus a crash-looping broker kept this from ever running (DBox,
# 2026-09-27) — the exact failure it exists to undo. It waits for the bus
# itself instead, bounded at 120s of uptime (the RTC can be hours off until
# chrony steps it); its .svc raises start_timeout_sec
# above the ~90s oneshot default, or PID 1 kills it mid-wait (also DBox).
set -u

LIB="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
FLIP="$LIB/schema-dbus-flip.sh"
STATE=/var/lib/schema-init/dbus-flip.state
COUNT=/var/lib/schema-init/dbus-flip-armed-boots
SVCDIR=/etc/schema-init/services
LOG=/var/log/schema-init/dbus-flip-healthcheck.log

[ "$(cat "$STATE" 2>/dev/null)" = armed ] || exit 0

log() { mkdir -p "$(dirname "$LOG")"; echo "[$(date -Is)] $*" >> "$LOG" 2>/dev/null; }

if [ ! -x "$FLIP" ]; then
    log "flip helper missing at $FLIP — cannot self-heal; leaving state untouched"
    exit 0
fi

rollback() {
    log "flip UNHEALTHY ($1) — rolling back to dbus-daemon"
    "$FLIP" rollback >> "$LOG" 2>&1 || log "rollback reported error"
    rm -f "$COUNT"
    log "rolled back; rebooting into safe state"
    schema-ctl reboot 2>/dev/null || reboot -f
    exit 0
}

has_owner() {
    dbus-send --system --print-reply --dest=org.freedesktop.DBus / \
        org.freedesktop.DBus.NameHasOwner "string:$1" 2>/dev/null | grep -q 'boolean true'
}

want=""
[ -f "$SVCDIR/schema-logind.svc" ]    && want="$want org.freedesktop.login1"
[ -f "$SVCDIR/network-manager.svc" ]  && want="$want org.freedesktop.NetworkManager"
[ -f "$SVCDIR/polkitd.svc" ]          && want="$want org.freedesktop.PolicyKit1"

missing=""
up() { cut -d. -f1 /proc/uptime; }
deadline=$(( $(up) + 120 ))
while [ "$(up)" -lt "$deadline" ]; do
    missing=""
    dbus-send --system --print-reply --dest=org.freedesktop.DBus / \
        org.freedesktop.DBus.GetId > /dev/null 2>&1 || missing="GetId"
    for n in $want; do
        has_owner "$n" || missing="$missing $n"
    done
    [ -z "$missing" ] && break
    sleep 1
done

# --- class 1 ---
pgrep -f '/schema-dbus --system$' > /dev/null || rollback "system bus is not schema-dbus (launcher fell back)"
[ -z "$missing" ] || rollback "system bus not serving:$missing"

# --- class 2 ---
n=$(cat "$COUNT" 2>/dev/null || echo 0)
case "$n" in ''|*[!0-9]*) n=0 ;; esac
n=$((n + 1))
echo "$n" > "$COUNT"
[ "$n" -ge 2 ] && rollback "desktop never confirmed across $n armed boots"

log "flip healthy (armed boot $n) — leaving armed; GUI will confirm at login"
exit 0
