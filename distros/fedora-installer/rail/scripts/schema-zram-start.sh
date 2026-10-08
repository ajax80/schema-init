#!/bin/bash
# schema-zram-start.sh — compressed-RAM swap under schema-init as PID 1.
# Fedora normally sets this up with systemd's zram-generator, which never runs
# here. A low-RAM box with no swap OOM-kills under load; a RAM-sized zstd zram
# device gives generous headroom for a fraction of the RAM (zstd ~2-3:1) and
# touches no disk. High priority so the kernel prefers it.
modprobe zram num_devices=1 2>/dev/null || true
DEV=/sys/block/zram0
[ -d "$DEV" ] || exit 0
# Timer mode (zram-recompress.svc): pages untouched since the last run move
# from fast lz4 to dense zstd, then everything is marked idle again.
if [ "$1" = recompress ]; then
    [ -e "$DEV/recomp_algorithm" ] && [ "$(cat "$DEV/disksize")" != "0" ] || exit 0
    echo type=idle > "$DEV/recompress" 2>/dev/null
    echo all > "$DEV/idle"
    exit 0
fi
# zswap in front of zram compresses every page twice.
echo N > /sys/module/zswap/parameters/enabled 2>/dev/null || true
# Make the kernel actually reach for it. Default swappiness (60) barely swaps
# while RAM is only moderately pressured, so a zram device just sits idle — the
# symptom "swap isn't being used." zram is near-free (RAM-backed, no seeks), so
# prefer it hard. These are exactly the values Fedora's zram-generator drops in
# 99-zram.conf, applied by hand because systemd-sysctl never runs under PID 1.
sysctl -qw vm.swappiness=180 vm.watermark_boost_factor=0 \
           vm.watermark_scale_factor=125 vm.page-cluster=0 2>/dev/null || true
# already configured (disksize nonzero)? leave it be — reconfiguring a live
# device fails EBUSY.
[ "$(cat "$DEV/disksize" 2>/dev/null)" != "0" ] && exit 0
# comp_algorithm must be set before disksize. lz4 reads back ~2.3x faster
# than zstd but packs ~30% looser; zstd recompression of idle pages (timer
# above) wins the density back. Kernels without recompression get plain zstd.
if [ -e "$DEV/recomp_algorithm" ] && echo lz4 > "$DEV/comp_algorithm" 2>/dev/null; then
    echo "algo=zstd priority=1" > "$DEV/recomp_algorithm" 2>/dev/null || echo zstd > "$DEV/comp_algorithm"
else
    echo zstd > "$DEV/comp_algorithm" 2>/dev/null || true
fi
awk '/MemTotal/{print $2*1024}' /proc/meminfo > "$DEV/disksize"
mkswap /dev/zram0 >/dev/null 2>&1
swapon -p 100 /dev/zram0
