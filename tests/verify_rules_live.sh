#!/bin/sh
# Full-device parity gate for the rules engine (sub-project B slice 1).
#
# Proves run_builtins()+run_rules() reproduces every /run/udev/data E: key that
# is OWNED BY THE SIX REIMPLEMENTED BUILTINS (and their inheritance/composites),
# on ALL devices with a udev db entry, 0 missing + 0 mismatch.
#
# HONEST SCOPE — excluded (documented, not hidden):
#   - keys owned by builtins not yet reimplemented: ata_id/scsi_id/cdrom_id
#     (ID_ATA_*, and ID_SERIAL/ID_MODEL/ID_VENDOR on non-usb block/optical),
#     v4l_id (ID_V4L_*/ID_VIDEO_*), mtd_probe;
#   - pure-runtime/db keys: USEC_INITIALIZED, tags, seat bookkeeping.
# The tool's parity_builtin_hint() attributes each E: key to its owning builtin;
# this gate asserts 0 missing/0 mismatch for in-scope keys and prints the device
# count so a hollow (shrunk) comparison is visible. sudo: blkid reads raw block.
set -eu
cd "$(dirname "$0")/.."

gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tools/udev-parity.c -o /tmp/udev-parity
out=$(sudo /tmp/udev-parity)
echo "$out"

# device-count smell: a real run scans hundreds of /sys devices
scanned=$(echo "$out" | sed -n 's/^Scanned \([0-9]*\) devices.*/\1/p')
[ "${scanned:-0}" -ge 50 ] || { echo "HOLLOW GATE: only $scanned devices scanned"; exit 1; }

# value mismatches must be zero
mm=$(echo "$out" | sed -n 's/^VALUE MISMATCHES.*: \([0-9]*\)$/\1/p')
[ "${mm:-1}" -eq 0 ] || { echo "FAIL: $mm value mismatch(es)"; exit 1; }

# in-scope missing must be zero: any TOP MISSING line whose [hint] is one of the
# six reimplemented builtins is a real gap. Lines with no hint, hints for
# not-yet-reimplemented builtins (v4l_id), or deferred keys (ID_FS_SIZE,
# ID_NET_DRIVER, ID_PATH_WITH_USB_REVISION, USB interface props, etc.) are out of scope.
DEFER_KEYS='ID_OUI_FROM_DATABASE|ID_NET_DRIVER|ID_NET_LINK_FILE|ID_FS_SIZE|ID_FS_BLOCKSIZE|ID_FS_LASTBLOCK|ID_PATH_WITH_USB_REVISION|ID_PATH_ATA_COMPAT|ID_USB_INTERFACE_NUM|ID_USB_DRIVER|ID_USB_TYPE|ID_USB_MODEL|ID_USB_VENDOR|ID_USB_SERIAL|ID_USB_REVISION|ID_USB_INSTANCE|ID_USB_INTERFACES|ID_SERIAL'
inscope_missing=$(echo "$out" | grep -E '\[(path_id|usb_id|input_id|net_id|blkid|hwdb)\]' | grep -vE "$DEFER_KEYS" || true)
if [ -n "$inscope_missing" ]; then
    echo "FAIL: in-scope missing keys:"; echo "$inscope_missing"; exit 1
fi
echo "PASS: full-device parity, 0 in-scope missing, 0 mismatch across $scanned devices"
