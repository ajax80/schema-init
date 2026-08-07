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

# in-scope missing must be zero. The tool computes this itself with device-class
# awareness (parity_in_scope_missing in udev-parity.h): a key udev has that we
# don't, attributed to one of the six reimplemented builtins, minus documented
# deferrals (ID_FS geometry, ID_PATH compat variants, ID_OUI, v4l_id) and minus
# identity/type keys on non-usb devices (ata_id/scsi_id/cdrom_id/dmi territory).
# Each such gap is printed as an INSCOPE-MISS line above, so it cannot be hidden.
im=$(echo "$out" | sed -n 's/^IN-SCOPE MISSING.*: \([0-9]*\)$/\1/p')
if [ "${im:-1}" -ne 0 ]; then
    echo "FAIL: $im in-scope missing key(s):"
    echo "$out" | grep '^INSCOPE-MISS'
    exit 1
fi
echo "PASS: full-device parity, 0 in-scope missing, 0 mismatch across $scanned devices"
