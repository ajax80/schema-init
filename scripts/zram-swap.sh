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

# Configure zram if not already configured
if [ "$(cat /sys/block/zram0/disksize 2>/dev/null || echo 0)" = "0" ]; then
    echo zstd > /sys/block/zram0/comp_algorithm || true
    echo 16G > /sys/block/zram0/disksize || true
fi

# Initialize and enable swap if not active
mkswap /dev/zram0
swapon --priority 100 /dev/zram0
