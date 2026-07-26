#!/bin/sh
# Heals the schema-init boot-order race: if KDE starts before the btrfs data
# mounts settle, plasmashell self-feeds a ksycoca/repaint loop (74% CPU, stutter).
# Wait for the mounts, then replace plasmashell ONLY if it's actually spinning.
# No-op (no flicker) on a clean boot. See project_schema_init_plasma_cpu_tax.

LOG=/tmp/kde-mount-guard.log
# Held only while `plasmashell --replace` is in flight; schema-plasma-watchdog
# defers its respawn while this exists. Keep the path in sync with the watchdog.
INHIBIT="${XDG_RUNTIME_DIR:-/tmp}/plasma-replace-inflight"
echo "guard start $(date)" > "$LOG"

# 1) wait up to 45s for every data mount to be present
for m in /home /mnt/XtraSpace /mnt/Space /mnt/MySpaceDuex; do
    i=0
    while ! mountpoint -q "$m"; do
        i=$((i + 1))
        [ "$i" -ge 90 ] && { echo "TIMEOUT waiting $m" >> "$LOG"; break; }
        sleep 0.5
    done
done
echo "mounts ready $(date)" >> "$LOG"

# 2) let plasmashell settle, then sample its instantaneous CPU
sleep 8
pid=$(pgrep -x plasmashell | head -1)
[ -z "$pid" ] && { echo "no plasmashell, exit" >> "$LOG"; exit 0; }
cpu=$(top -bn2 -d2 -p "$pid" 2>/dev/null | awk -v p="$pid" '$1==p{c=$9} END{print int(c+0)}')
echo "plasmashell pid=$pid cpu=$cpu" >> "$LOG"

# 3) replace only if it's stuck spinning (clean settles ~10-15%, race pegs ~70%)
if [ "${cpu:-0}" -gt 40 ]; then
    echo "SPINNING -> plasmashell --replace" >> "$LOG"

    # 4) the --replace handoff is a D-Bus name race: the old instance drops
    # org.kde.plasmashell and the new one reacquires it, so there is a window
    # with no plasmashell at all. schema-plasma-watchdog polls every 5s and
    # reads that window as a crash -- its restart then races ours for the name
    # and BOTH instances exit, leaving KWin up and the screen black with a live
    # cursor (seen 07-26 12:03). Inhibit the watchdog across the handoff: it
    # owns respawn, we only own the replace. Trap so a kill can't strand it.
    trap 'rm -f "$INHIBIT"' EXIT INT TERM
    : > "$INHIBIT"

    plasmashell --replace >/dev/null 2>&1 &

    # Settle first -- polling too early just finds the OLD instance winding down.
    sleep 10
    i=0
    while [ "$i" -lt 20 ]; do
        pgrep -x plasmashell >/dev/null && break
        i=$((i + 1))
        sleep 1
    done
    rm -f "$INHIBIT"

    if pgrep -x plasmashell >/dev/null; then
        echo "replace ok, plasmashell pid=$(pgrep -x plasmashell | head -1)" >> "$LOG"
    else
        # Deliberately do NOT start one here. The inhibit is down, so the
        # watchdog sees no plasmashell within 5s and respawns it with its own
        # crash-loop backoff. A restart from us would just be the second racer.
        echo "REPLACE LOST THE NAME -> handing off to watchdog $(date)" >> "$LOG"
    fi
fi
