#!/bin/sh
# Load system and user profiles to establish proper environment variables (PATH, XDG_DATA_DIRS, etc.)
if [ -f /etc/profile ]; then
    . /etc/profile
fi
if [ -f "$HOME/.profile" ]; then
    . "$HOME/.profile"
elif [ -f "$HOME/.bash_profile" ]; then
    . "$HOME/.bash_profile"
fi

/usr/bin/startplasma-wayland &
SPW=$!
i=0
while [ $i -lt 60 ]; do
    [ -S ${XDG_RUNTIME_DIR}/wayland-0 ] && break
    sleep 0.5
    i=$((i+1))
done
export WAYLAND_DISPLAY=wayland-0
i=0
while [ $i -lt 20 ]; do
    [ -S ${XDG_RUNTIME_DIR}/pipewire-0 ] && break
    sleep 0.5
    i=$((i+1))
done
sleep 1
pgrep -x plasmashell > /dev/null || /usr/local/bin/plasmashell-shim &
wait $SPW
