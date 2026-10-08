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

exec prlimit --rtprio=70 --nice=39 --memlock=4294967296 -- setpriv --reuid=daedalus --regid="$(id -g daedalus)" --init-groups -- env HOME="$(getent passwd daedalus | cut -d: -f6)" USER=daedalus LOGNAME=daedalus XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" /usr/bin/pipewire
