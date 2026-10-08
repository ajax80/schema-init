#!/bin/sh
i=0
while [ $i -lt 10 ] && ! arecord -l 2>/dev/null | grep -q "USB Audio"; do
    sleep 1
    i=$((i+1))
done

exec /usr/bin/pipewire
