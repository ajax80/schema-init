#!/bin/sh
/usr/bin/startplasma-wayland &
SPW=$!
i=0
while [ $i -lt 60 ]; do
    [ -S "${XDG_RUNTIME_DIR}/wayland-0" ] && break
    sleep 0.5
    i=$((i+1))
done
i=0
while [ $i -lt 20 ]; do
    [ -S "${XDG_RUNTIME_DIR}/pipewire-0" ] && break
    sleep 0.5
    i=$((i+1))
done
sleep 1
pgrep -x plasmashell > /dev/null || plasmashell &
wait $SPW
