#!/bin/sh
# stage.sh MODE   (MODE=hup | MODE=restart)
#
# Runs INSIDE the VM guest, from the 9p share. Stands up the bridge plus a fake
# compositor that holds the DRM node, then reloads the bridge either the new way
# (SIGHUP re-exec) or the old way (kill + respawn), and leaves the VM parked so
# the host can inject ctrl-alt-f8 and photograph the result.
#
# Writes everything to /mnt/out so the host reads results from files instead of
# screen-scraping a console over sendkey.
MODE=${1:?usage: stage.sh hup|restart}
OUT=/mnt/out
mkdir -p "$OUT"
exec >"$OUT/$MODE.log" 2>&1
set -x

DRM=226:0
VT=2

# The ISO ships its own bridge as a supervised service, so a plain pkill just
# makes PID 1 respawn it -- and the respawn takes the bus name back with
# REPLACE_EXISTING, so the fake compositor ends up driving the OLD code while
# this script reports on the new one. Move the binary out from under the
# supervisor first: the restarts then fail and the service falls to DORMANT.
if [ -f /usr/local/bin/schema-logind.py ]; then
    mv /usr/local/bin/schema-logind.py /usr/local/bin/schema-logind.py.off
fi
pkill -f schema-logind
# A compositor left over from an earlier mode still holds the DRM node and has
# already spent its TakeControl on a dead bridge -- it would both keep master
# and shadow the new one in pgrep.
pkill -f fake-compositor
sleep 8
echo "ISO_BRIDGE_LEFT=$(pgrep -cf '[s]chema-logind')"
echo "STALE_COMP_LEFT=$(pgrep -cf '[f]ake-compositor')"

start_bridge() {
    SCHEMA_LOGIND_VTNR=$VT setsid python3 /mnt/schema-logind.py \
        >>"$OUT/$MODE-bridge.log" 2>&1 &
    sleep 3
}

start_bridge
BPID=$(pgrep -f "[s]chema-logind.py" | head -1)
echo "BRIDGE_BEFORE=$BPID"

setsid python3 /mnt/fake-compositor.py "$DRM" >"$OUT/$MODE-comp.log" 2>&1 &
sleep 3
echo "COMP=$(pgrep -f '[f]ake-compositor.py' | head -1)"
# Proves the compositor reached THIS bridge and not some other owner of the
# name. Without it a mis-wired rig reports a clean run against the wrong code.
echo "TAKECONTROL_SEEN=$(grep -c 'TakeControl(0) requested' "$OUT/$MODE-bridge.log")"
echo "MEDIATED_BEFORE=$(grep -c 'now mediated' "$OUT/$MODE-bridge.log")"

if [ "$MODE" = hup ]; then
    kill -HUP "$BPID"
    sleep 3
else
    kill "$BPID"
    sleep 2
    start_bridge
fi

AFTER=$(pgrep -f "[s]chema-logind.py" | head -1)
echo "BRIDGE_AFTER=$AFTER"
echo "PID_STABLE=$([ "$AFTER" = "$BPID" ] && echo yes || echo no)"
echo "ADOPTED=$(grep -c 'adopted handoff' "$OUT/$MODE-bridge.log")"
echo "MEDIATED_TOTAL=$(grep -c 'now mediated' "$OUT/$MODE-bridge.log")"
echo "ACTIVE_VT=$(cat /sys/class/tty/tty0/active)"
echo STAGED > "$OUT/$MODE.ready"
