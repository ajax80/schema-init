#!/bin/sh
exec >> /var/log/network-up.log 2>&1
printf "network-up start: %s\n" "$(date)"
modprobe r8152 2>&1 || true
modprobe r8169 2>&1 || true
modprobe ath9k 2>&1 || true
modprobe snd_hda_intel 2>&1 || true
rm -f /etc/resolv.conf
printf "nameserver 192.168.8.1\nnameserver 8.8.8.8\n" > /etc/resolv.conf
udevadm trigger --subsystem-match=usb 2>/dev/null || true
udevadm settle --timeout=10 2>/dev/null || true
udevadm trigger --subsystem-match=net 2>/dev/null || true
udevadm settle --timeout=5 2>/dev/null || true
printf "interfaces after modprobe:\n"
ip link show 2>&1
printf "network-up done\n"
exit 0
