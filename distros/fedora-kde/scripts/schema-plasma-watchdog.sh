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

echo "watchdog start $(date)" > "$WD_LOG"
fails=0; window=$(date +%s)
while sleep 5; do
    pgrep -x kwin_wayland >/dev/null 2>&1 || { echo "compositor gone, exit $(date)" >>"$WD_LOG"; break; }
    pgrep -x plasmashell  >/dev/null 2>&1 && continue
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
