#!/bin/sh
# nvidia-modules: explicitly load NVIDIA driver stack at boot. On the default
# (initramfs) entry these are loaded early; on the NO-initramfs SchemaKernel
# entry nothing loads them before the display server, so schema-init must.
# Idempotent: skip if /dev/nvidia0 already present (udev coldplug may beat us).
if [ -c /dev/nvidia0 ]; then
    printf "nvidia already loaded.\\n"
    exit 0
fi
modprobe nvidia          2>/dev/null || true
modprobe nvidia_uvm      2>/dev/null || true
modprobe nvidia_modeset  2>/dev/null || true
modprobe nvidia_drm      modeset=1 2>/dev/null || true
for i in $(seq 1 30); do
    [ -c /dev/nvidia0 ] && break
    sleep 0.1
done
[ -c /dev/nvidia0 ] || printf "nvidia0 not present after modprobe!\\n" >&2
exit 0
