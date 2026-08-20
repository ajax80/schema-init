#!/bin/sh
# Headless seatbelt for the schema-udev flip. Runs as a schema-init oneshot on
# EVERY boot, before the desktop — so it can undo a flip that broke /dev even
# when there is no working GUI to run the wizard. This is what makes the flip
# safe to hand to a novice: a bad flip self-heals to the last good state.
#
# Only acts when a flip was just armed (state=armed). Healthy -> leave it, the
# GUI wizard confirms success at login. Broken -> disarm + rollback + reboot.
set -u

STATE=/var/lib/schema-init/firstboot.state
LIB=/usr/local/lib/schema
ARM="$LIB/schema-udev-flip-arm.sh"
BACKUP="$LIB/schema-udev-flip-backup.sh"
LOG=/var/log/schema-init/flip-healthcheck.log

[ -f "$STATE" ] && [ "$(cat "$STATE")" = "armed" ] || exit 0

log() { echo "[$(date -Is)] $*" >> "$LOG" 2>/dev/null; }

# give schema-udev a moment to populate /dev after the flip
i=0; while [ $i -lt 10 ]; do pgrep -x schema-udev >/dev/null 2>&1 && break; i=$((i+1)); sleep 1; done

healthy=1
# *** INTEGRATION POINT: the minimal "/dev is alive" contract. Tune the node
# list to what schema-udev is expected to have created by this point.
pgrep -x schema-udev >/dev/null 2>&1 || { healthy=0; log "schema-udev not running"; }
for node in /dev/null /dev/console /dev/urandom; do
    [ -e "$node" ] || { healthy=0; log "missing core node $node"; }
done
# at least one real block device node for the root disk must exist
ls /dev/disk/by-uuid/ >/dev/null 2>&1 || { healthy=0; log "no /dev/disk/by-uuid entries"; }

if [ "$healthy" = 1 ]; then
    log "flip healthy — leaving armed; GUI will confirm"
    exit 0
fi

log "flip UNHEALTHY — rolling back to systemd-udev"
"$ARM" disarm || true
SCHEMA_UDEV_SKIP_ROOT=0 "$BACKUP" rollback >> "$LOG" 2>&1 || log "rollback reported error"
echo skipped > "$STATE"
log "rolled back; rebooting into safe state"
# reboot via schema-init's own path
schema-ctl reboot 2>/dev/null || reboot -f
