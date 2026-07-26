#!/bin/bash
# schema-init Track B: plasmashell watchdog.
# No `systemd --user` to respawn plasmashell, so a crash leaves the launcher and
# system tray dead -- apps then "open once, then refuse" because click-to-launch
# and the tray host live inside plasmashell. Keep it alive while the Wayland
# session lives; exit cleanly once kwin_wayland is gone (logout).
#
# MUST be launched detached (setsid nohup) from schema-autostart-runner.sh:
# the runner is a setsid session leader, so a bare `( ) &` child takes SIGHUP
# when the runner exits and dies before it can ever respawn anything.
WD_LOG=/tmp/schema-plasma-watchdog.log

# Single instance: never let two watchdogs run (both would respawn plasmashell).
LOCK="${XDG_RUNTIME_DIR:-/tmp}/schema-plasma-watchdog.lock"
exec 9>"$LOCK"
flock -n 9 || { echo "another watchdog already running, exit $(date)" >> "$WD_LOG"; exit 0; }

# kde-mount-guard raises this while `plasmashell --replace` is in flight.
# Keep the path in sync with that script.
INHIBIT="${XDG_RUNTIME_DIR:-/tmp}/plasma-replace-inflight"

echo "watchdog start $(date)" > "$WD_LOG"
fails=0; window=$(date +%s)
while sleep 5; do
    pgrep -x kwin_wayland >/dev/null 2>&1 || { echo "compositor gone, exit $(date)" >>"$WD_LOG"; break; }
    pgrep -x plasmashell  >/dev/null 2>&1 && continue

    # A --replace is mid-handoff: the D-Bus name is briefly held by nobody and
    # no plasmashell exists. Restarting into that window makes a second racer
    # and both instances lose the name. Ignore a stale flag so a guard that was
    # killed mid-replace can never wedge respawn off for the whole session.
    if [ -f "$INHIBIT" ]; then
        age=$(( $(date +%s) - $(stat -c %Y "$INHIBIT" 2>/dev/null || echo 0) ))
        if [ "$age" -lt 60 ]; then
            echo "replace in flight (${age}s), deferring $(date)" >>"$WD_LOG"
            continue
        fi
        echo "stale inhibit flag (${age}s), ignoring $(date)" >>"$WD_LOG"
    fi

    now=$(date +%s)
    [ $((now - window)) -gt 60 ] && { fails=0; window=$now; }
    fails=$((fails + 1))
    if [ "$fails" -gt 5 ]; then
        echo "plasmashell crash-looping (>5/60s), backing off 60s $(date)" >>"$WD_LOG"
        sleep 60; fails=0; window=$(date +%s); continue
    fi
    echo "plasmashell down, restart #$fails $(date)" >>"$WD_LOG"
    setsid nohup plasmashell >/dev/null 2>&1 &
    sleep 3
done
