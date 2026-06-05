#!/bin/sh
case "$1" in
    bound|renew)
        ip addr flush dev "$interface" 2>/dev/null
        ip addr add "$ip/$mask" dev "$interface" 2>/dev/null
        ip link set "$interface" up
        for r in $router; do
            ip route add default via "$r" dev "$interface" 2>/dev/null || true
        done
        [ -n "$dns" ] && printf 'nameserver %s\n' $dns > /etc/resolv.conf
        ;;
    deconfig)
        ip addr flush dev "$interface" 2>/dev/null
        ip route del default 2>/dev/null || true
        ;;
esac
