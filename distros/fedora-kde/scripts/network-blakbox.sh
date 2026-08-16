#!/bin/sh
exec >> /var/log/network-blakbox.log 2>&1
ip link set eth0 up
exec busybox udhcpc -f -i eth0 -s /usr/local/bin/udhcpc.sh
