#!/bin/bash
# schema-zram-start.sh — compressed-RAM swap under schema-init as PID 1.
# Fedora normally sets this up with systemd's zram-generator, which never runs
# here. A low-RAM box with no swap OOM-kills under load; a RAM-sized zstd zram
# device gives generous headroom for a fraction of the RAM (zstd ~2-3:1) and
# touches no disk. High priority so the kernel prefers it.
modprobe zram num_devices=1 2>/dev/null || true
DEV=/sys/block/zram0
[ -d "$DEV" ] || exit 0
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
# comp_algorithm must be set before disksize.
echo zstd > "$DEV/comp_algorithm" 2>/dev/null || true
awk '/MemTotal/{print $2*1024}' /proc/meminfo > "$DEV/disksize"
mkswap /dev/zram0 >/dev/null 2>&1
swapon -p 100 /dev/zram0
