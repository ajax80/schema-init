#!/bin/sh
/usr/bin/startplasma-wayland &
SPW=$!
i=0
while [ $i -lt 60 ]; do
    [ -S "${XDG_RUNTIME_DIR}/wayland-0" ] && break
    sleep 0.5
    i=$((i+1))
done
sleep 2
pgrep -x plasmashell > /dev/null || plasmashell &
wait $SPW
