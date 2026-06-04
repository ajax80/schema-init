#!/bin/sh
export XDG_RUNTIME_DIR=/run/user/1000
mkdir -p "$XDG_RUNTIME_DIR"
chown 1000:1000 "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

i=0
while [ $i -lt 10 ] && ! arecord -l 2>/dev/null | grep -q "USB Audio"; do
    sleep 1
    i=$((i+1))
done

exec runuser -u daedalus -- env XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" /usr/bin/pipewire
