#!/bin/sh
set -e

# zswap in front of zram compresses every page twice
echo N > /sys/module/zswap/parameters/enabled 2>/dev/null || true

# Timer mode (zram-recompress.svc): pages untouched since the last run move
# from lz4 to zstd, then everything is marked idle again
if [ "${1:-}" = recompress ]; then
    B=/sys/block/zram0
    [ -e $B/recomp_algorithm ] && [ "$(cat $B/disksize 2>/dev/null || echo 0)" != "0" ] || exit 0
    echo type=idle > $B/recompress 2>/dev/null || true
    echo all > $B/idle
    exit 0
fi

# If already active swap, exit successfully
if grep -q "/zram0[[:space:]]" /proc/swaps; then
    printf "/dev/zram0 is already active swap.\n"
    exit 0
fi

# Load zram module
modprobe zram num_devices=1

# Wait for /dev/zram0 to appear
for i in $(seq 1 20); do
    if [ -b /dev/zram0 ]; then
        break
    fi
    sleep 0.1
done

if [ ! -b /dev/zram0 ]; then
    printf "zram0 block device not found!\n" >&2
    exit 1
fi

# Size = total RAM by default; override with ZRAM_SIZE (e.g. ZRAM_SIZE=8G)
if [ -z "${ZRAM_SIZE:-}" ]; then
    ZRAM_SIZE="$(awk '/MemTotal/{printf "%d", $2*1024}' /proc/meminfo)"
fi

# Configure zram
if [ -e /sys/block/zram0/recomp_algorithm ] && echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null; then
    echo "algo=zstd priority=1" > /sys/block/zram0/recomp_algorithm 2>/dev/null || echo zstd > /sys/block/zram0/comp_algorithm
else
    echo zstd > /sys/block/zram0/comp_algorithm
fi
echo "$ZRAM_SIZE" > /sys/block/zram0/disksize

# Initialize and enable swap
mkswap /dev/zram0
swapon --priority 100 /dev/zram0
