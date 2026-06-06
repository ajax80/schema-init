#!/bin/sh
case $1 in
    bound|renew)
        ip addr flush dev $interface 2>/dev/null
        ip addr add $ip/$mask dev $interface 2>/dev/null
        ip link set $interface up
        for r in $router; do
            ip route add default via $r dev $interface 2>/dev/null || true
        done
        printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > /etc/resolv.conf
        ;;
    deconfig)
        ip addr flush dev $interface 2>/dev/null
        ip route del default 2>/dev/null || true
        ;;
esac
