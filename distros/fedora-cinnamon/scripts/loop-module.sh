#!/bin/sh
# loop-module: load the loop driver at boot so disk images (.iso/.img) can be
# mounted. On systemd this is udev-autoloaded on first use; schema-init loads it
# explicitly. Idempotent.

if [ -c /dev/loop-control ]; then
    printf "loop already loaded.\\n"
    exit 0
fi

modprobe loop max_loop=16

for i in $(seq 1 20); do
    [ -c /dev/loop-control ] && break
    sleep 0.1
done

if [ ! -c /dev/loop-control ]; then
    printf "loop-control not present after modprobe loop!\\n" >&2
    exit 1
fi
