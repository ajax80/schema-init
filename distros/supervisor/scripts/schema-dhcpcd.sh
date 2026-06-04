#!/bin/sh
mkdir -p /var/log/schema-init
exec /usr/sbin/dhcpcd -B -f /dev/null wlan0 >>/var/log/schema-init/dhcpcd.log 2>&1
