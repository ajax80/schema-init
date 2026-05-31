#!/bin/sh
exec >> /var/log/polkitd.log 2>&1
printf "polkitd-wrapper start: %s\n" "$(date)"

/usr/lib/polkit-1/polkitd --no-debug
sleep 1

PID=$(pgrep -n polkitd 2>/dev/null)
printf "polkitd-wrapper: tracking orphan pid %s\n" "$PID"

while [ -n "$PID" ] && [ -d "/proc/$PID" ]; do
    sleep 5
done

printf "polkitd-wrapper: polkitd gone, exiting\n"
