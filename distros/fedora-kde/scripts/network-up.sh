#!/bin/sh
exec >> /var/log/network-up.log 2>&1
printf "network-up start: %s\n" "$(date)"
modprobe r8152 2>&1 || true
udevadm trigger --subsystem-match=usb 2>/dev/null || true
udevadm settle --timeout=10 2>/dev/null || true
udevadm trigger --subsystem-match=net 2>/dev/null || true
udevadm settle --timeout=5 2>/dev/null || true
rm -f /etc/resolv.conf
printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > /etc/resolv.conf
printf "interfaces after modprobe:\n"
ip link show 2>&1
IFACE=$(ip -o link show | awk -F': ' '{print $2}' | grep -v '^lo$' | grep -v '^wl' | head -1)
if [ -z "$IFACE" ]; then
    printf "no ethernet interface found\n"
    exit 0
fi
printf "configuring: %s\n" "$IFACE"
ip link set "$IFACE" up 2>&1
ip addr add 192.168.8.246/24 dev "$IFACE" 2>&1 || true
ip route add default via 192.168.8.1 2>&1 || true
printf "network-up done\n"
exit 0
