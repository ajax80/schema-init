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

# DETERMINISTIC node creation. nvidia-modprobe -c 0 races the PCI bind on the
# no-initramfs boot: it returns 0 having enumerated ZERO GPUs, so it mknods
# neither /dev/nvidia0 nor /dev/nvidiactl and the compositor falls back to
# simpledrm (proven: two cold boots, 5x "nvidia0 absent", nodes only appeared
# after a hand mknod). Unlike modeset/uvm (kernel miscdevices, auto-created),
# the frontend nodes need userspace mknod, so do it ourselves from the
# registered major -- the exact recovery that fixes every degraded boot by hand.
NVMAJ=$(awk '$2=="nvidia"{print $1; exit}' /proc/devices)
[ -n "$NVMAJ" ] || NVMAJ=195
[ -c /dev/nvidiactl ] || { mknod -m 666 /dev/nvidiactl c "$NVMAJ" 255 && log "mknod /dev/nvidiactl ($NVMAJ,255)"; }
[ -c /dev/nvidia0 ]   || { mknod -m 666 /dev/nvidia0   c "$NVMAJ" 0   && log "mknod /dev/nvidia0 ($NVMAJ,0)"; }

# confirm (immediate now that we mknod ourselves; the wait only covers a slow
# major registration right after modprobe).
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
