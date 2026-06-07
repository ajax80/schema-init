#!/bin/sh
set -e

# If already active swap, exit successfully
if grep -q "^/dev/zram0" /proc/swaps; then
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
echo zstd > /sys/block/zram0/comp_algorithm
echo "$ZRAM_SIZE" > /sys/block/zram0/disksize

# Initialize and enable swap
mkswap /dev/zram0
swapon --priority 100 /dev/zram0
