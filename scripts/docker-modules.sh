#!/bin/sh
# Load kernel modules required by docker
modprobe overlay 2>/dev/null || true
modprobe br_netfilter 2>/dev/null || true
modprobe nf_nat 2>/dev/null || true

# Set required sysctls
echo 1 > /proc/sys/net/ipv4/ip_forward
echo 1 > /proc/sys/net/bridge/bridge-nf-call-iptables 2>/dev/null || true
