#!/bin/sh
mkdir -p /run/wpa_supplicant
mkdir -p /var/log/schema-init
rfkill unblock wifi
udevadm trigger
i=0
while [ $i -lt 30 ] && ! ip link show wlan0 >/dev/null 2>&1; do
    sleep 1
    i=$((i+1))
done
exec /usr/sbin/wpa_supplicant -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant-wlan0.conf -O /run/wpa_supplicant >>/var/log/schema-init/wpa.log 2>&1
