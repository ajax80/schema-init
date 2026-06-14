#!/bin/sh
# nvidia-modules: explicitly load NVIDIA driver stack at boot. On the default
# (initramfs) entry these are loaded early; on the NO-initramfs SchemaKernel
# entry nothing loads them before the display server, so schema-init must.
# Idempotent: skip if /dev/nvidia0 already present (udev coldplug may beat us).
#
# Logs every run to a persistent file so a reboot can prove WHETHER this ran
# and WHEN nvidia became available relative to the compositor. Loading nvidia
# after kwin is already up (on simpledrm) forces a Wayland compositor restart
# that kills every client -- so getting this in BEFORE sddm is the whole point.
LOG=/var/log/schema-init/nvidia-modules.log
[ -d /var/log/schema-init ] || LOG=/run/nvidia-modules.log
log() { printf '%s nvidia-modules: %s\n' "$(date '+%F %T')" "$1" >> "$LOG"; }

if [ -c /dev/nvidia0 ]; then
    log "already loaded (udev coldplug beat us)"
    exit 0
fi
log "loading modules (no /dev/nvidia0 yet)"
for m in nvidia nvidia_uvm nvidia_modeset "nvidia_drm modeset=1"; do
    # shellcheck disable=SC2086
    err=$(modprobe $m 2>&1) || log "modprobe $m FAILED: $err"
done
for i in $(seq 1 30); do
    [ -c /dev/nvidia0 ] && break
    sleep 0.1
done
if [ -c /dev/nvidia0 ]; then
    log "ready: /dev/nvidia0 present after modprobe"
    exit 0
fi
log "FAILED: /dev/nvidia0 absent after modprobe (compositor will come up on simpledrm)"
exit 1
