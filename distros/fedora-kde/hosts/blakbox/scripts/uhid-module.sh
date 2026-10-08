#!/bin/sh
if [ -c /dev/uhid ]; then
    printf "uhid already loaded.\n"
    exit 0
fi

modprobe uhid

for i in $(seq 1 20); do
    [ -c /dev/uhid ] && break
    sleep 0.1
done

if [ ! -c /dev/uhid ]; then
    printf "uhid not present after modprobe!\n" >&2
    exit 1
fi
