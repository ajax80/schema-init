#!/bin/sh
# Mount the Ocean branch drives by UUID (STABLE). Do NOT use /dev/sdX here:
# kernel device names are assigned by detection order and change across boots,
# which swapped /mnt/Space <-> /mnt/MySpaceDuex and made qBittorrent (and game)
# absolute paths point at the wrong physical drive. UUIDs match Fedora's fstab.
#
# On the NO-initramfs boot the SATA/USB disks are still enumerating when this
# service runs, so a bare `mount UUID=` failed "can't find UUID". Wait for the
# udev by-uuid node to appear (up to ~30s/drive) before mounting.
LOG=/var/log/schema-init/mount-ocean-drives.log
[ -d /var/log/schema-init ] || LOG=/run/mount-ocean-drives.log
log() { printf '%s mount-ocean-drives: %s\n' "$(date '+%F %T')" "$1" >> "$LOG"; }


mount_uuid() {
    uuid=$1; target=$2
    mountpoint -q "$target" && return 0
    # Retry the mount itself, do NOT gate on /dev/disk/by-uuid/$uuid: schema-init
    # runs this oneshot in PID1's mount namespace, whose /dev (schema-udev owned)
    # does not expose the by-uuid symlinks the session sees, so `[ -e ... ]` is
    # ALWAYS false here and every drive burned its full timeout before mounting
    # anyway (extdrive at boot+6min, 2026-08-18). `mount UUID=` resolves via a
    # blkid scan that DOES work in this ns, so retry it directly -> mounts within
    # seconds of the device appearing. (Claire 2026-08-18)
    i=0
    while [ "$i" -lt 300 ]; do
        if mount UUID="$uuid" "$target" 2>/dev/null; then
            log "mounted $target ($uuid)"
            return 0
        fi
        i=$((i + 1)); sleep 0.5
    done
    log "FAILED $target ($uuid) — mount failed after 150s"
}

mount_uuid 1b7d654f-f578-499a-8288-ee40e43af3cb /mnt/Space        # label "Space"
mount_uuid e841ba0a-d7b9-42b6-b627-8ea27df85a54 /mnt/MySpaceDuex  # label "OuterSpace"
mount_uuid cee125d7-3f78-49dd-9e1c-c7689a64aff1 /mnt/XtraSpace    # label "XtraSpace"
mount_uuid 225f3963-bef6-4f17-9bd6-453bf524b8a9 /mnt/SeaGate     # SeaGate4Tb (internal via ASM1166 since 2026-10-04)
mount_uuid 256f9993-d3bf-41fb-bff1-0975b67fdcb3 /mnt/VostroSpace # label "VostroSpace"
