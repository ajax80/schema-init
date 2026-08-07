#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev parity

sudo rm -rf /run/schema-udev
sudo ./schema-udev &
UDPID=$!
sleep 2
sudo kill "$UDPID" 2>/dev/null || true
wait "$UDPID" 2>/dev/null || true

# 1) parity tool: db counters must be exactly 0
OUT=$(sudo ./udev-parity)
echo "$OUT" | grep -E 'IN-SCOPE MISSING \(db\)|VALUE MISMATCHES \(db\)'
MISS=$(echo "$OUT" | sed -n 's/^IN-SCOPE MISSING (db): //p')
MMIS=$(echo "$OUT" | sed -n 's/^VALUE MISMATCHES (db): //p')
[ "$MISS" = "0" ] || { echo "FAIL: db in-scope missing=$MISS"; exit 1; }
[ "$MMIS" = "0" ] || { echo "FAIL: db value mismatches=$MMIS"; exit 1; }

# 2) no phantom shadow files: every shadow record names a real udevd record
PHANTOM=0
for f in /run/schema-udev/data/*; do
    [ -e "$f" ] || continue
    key=$(basename "$f")
    [ -e "/run/udev/data/$key" ] || { echo "PHANTOM: $key has no /run/udev/data counterpart"; PHANTOM=$((PHANTOM+1)); }
done
[ "$PHANTOM" = "0" ] || { echo "FAIL: $PHANTOM phantom shadow records"; exit 1; }

echo ">> RESULT: PASS (db live gate)"
