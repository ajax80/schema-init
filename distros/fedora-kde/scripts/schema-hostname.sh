#!/bin/sh
[ -r /etc/hostname ] || exit 0
hn=$(cat /etc/hostname)
[ -n "$hn" ] && hostname "$hn"
exit 0
