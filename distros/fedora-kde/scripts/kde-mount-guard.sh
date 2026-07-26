#!/bin/sh
# Heals the schema-init boot-order race: if KDE starts before the btrfs data
# mounts settle, plasmashell self-feeds a ksycoca/repaint loop (74% CPU, stutter).
# Wait for the mounts, then replace plasmashell ONLY if it's actually spinning.
# No-op (no flicker) on a clean boot. See project_schema_init_plasma_cpu_tax.

LOG=/tmp/kde-mount-guard.log
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
    plasmashell --replace >/dev/null 2>&1 &

    # 4) the --replace handoff is a D-Bus name race: the old instance drops
    # org.kde.plasmashell, and if the new one loses the race to reacquire it,
    # it logs "Failed to register name" and exits -- leaving NO shell at all.
    # KWin survives, so the screen goes black with a live cursor (seen 07-26).
    # Poll for a survivor; if there is none, start one bare. Settle first --
    # polling too early just finds the OLD instance still winding down.
    sleep 10
    i=0
    while [ "$i" -lt 20 ]; do
        pgrep -x plasmashell >/dev/null && break
        i=$((i + 1))
        sleep 1
    done
    if pgrep -x plasmashell >/dev/null; then
        echo "replace ok, plasmashell pid=$(pgrep -x plasmashell | head -1)" >> "$LOG"
    else
        echo "REPLACE LOST THE NAME -> bare restart $(date)" >> "$LOG"
        plasmashell >/dev/null 2>&1 &
        sleep 10
        if pgrep -x plasmashell >/dev/null; then
            echo "recovered, pid=$(pgrep -x plasmashell | head -1)" >> "$LOG"
        else
            echo "STILL NO PLASMASHELL -- black screen, needs hands" >> "$LOG"
        fi
    fi
fi
