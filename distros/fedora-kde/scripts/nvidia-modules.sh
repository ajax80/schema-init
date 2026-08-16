#!/bin/sh
# nvidia-modules: create the NVIDIA device nodes + load the driver stack at boot.
# On the NO-initramfs SchemaKernel entry nothing loads them before the display
# server, so schema-init must. Loading nvidia after kwin is already up (on
# simpledrm) forces a Wayland compositor restart that kills every client --
# getting this in BEFORE sddm is the whole point.
#
# Node creation is done by nvidia-modprobe (the setuid, distro-independent helper
# whose whole job is this), NOT by waiting for udev to react to the module load.
# `modprobe` loads modules but does not mknod /dev/nvidia0; betting on a udev
# coldplug race is what produced the die/respawn/dormant boots (the service
# exited 1 on a 30s timeout until schema-init stopped retrying, then no nvidia0).
# Doing the mknod ourselves is deterministic AND udev-independent, so it survives
# the schema-udev flip regardless of whether schema-udev creates the node.
LOG=/var/log/schema-init/nvidia-modules.log
[ -d /var/log/schema-init ] || LOG=/run/nvidia-modules.log
log() { printf '%s nvidia-modules: %s\n' "$(date '+%F %T')" "$1" >> "$LOG"; }

if [ -x /usr/bin/nvidia-modprobe ]; then
    # -c 0: load nvidia + create /dev/nvidia0 (and /dev/nvidiactl)
    # -u  : load nvidia_uvm + create /dev/nvidia-uvm*
    # -m  : load nvidia_modeset + create /dev/nvidia-modeset (frigate CDI needs it)
    # idempotent: no-op exit 0 when already loaded (verified on live box).
    err=$(/usr/bin/nvidia-modprobe -c 0 -u -m 2>&1) || log "nvidia-modprobe -c 0 -u -m FAILED: $err"
else
    log "WARN: /usr/bin/nvidia-modprobe missing -- falling back to modprobe (udev must create nodes)"
    for m in nvidia nvidia_uvm nvidia_modeset; do
        # shellcheck disable=SC2086
        err=$(modprobe $m 2>&1) || log "modprobe $m FAILED: $err"
    done
fi
# nvidia_drm (KMS for Wayland) is the one module nvidia-modprobe does not handle.
err=$(modprobe nvidia_drm modeset=1 2>&1) || log "modprobe nvidia_drm modeset=1 FAILED: $err"

# nvidia-modprobe creates the nodes synchronously; this short bounded wait only
# matters for the modprobe-fallback path above.
for i in $(seq 1 50); do
    [ -c /dev/nvidia0 ] && break
    sleep 0.1
done
if [ ! -c /dev/nvidia0 ]; then
    log "FAILED: /dev/nvidia0 absent (compositor will come up on simpledrm)"
    exit 1
fi
log "ready: /dev/nvidia0 present"
if [ -c /dev/nvidia-modeset ]; then
    log "ready: /dev/nvidia-modeset present"
else
    log "WARN: /dev/nvidia-modeset absent (frigate will retry; login unaffected)"
fi
exit 0
