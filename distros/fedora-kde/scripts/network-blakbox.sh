#!/bin/sh
exec >> /var/log/network-blakbox.log 2>&1
ip link set enp6s0 up
exec busybox udhcpc -f -i enp6s0 -s /usr/local/bin/udhcpc.sh
