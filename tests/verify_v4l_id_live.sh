#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev parity

OUT=$(sudo ./udev-parity)
MM=$(echo "$OUT" | sed -n 's/^VALUE MISMATCHES (keys in both, differing value): //p')
MISS=$(echo "$OUT" | sed -n 's/^IN-SCOPE MISSING (device-class aware): //p')
echo "mismatches=$MM inscope-missing=$MISS"
[ "$MM" = "0" ] || { echo "FAIL: $MM value mismatches"; echo "$OUT" | grep '^VALMIS'; exit 1; }
[ "$MISS" = "0" ] || { echo "FAIL: $MISS in-scope missing"; echo "$OUT" | grep '^INSCOPE-MISS'; exit 1; }

sudo rm -rf /run/schema-udev
sudo ./schema-udev & UDPID=$!
sleep 2; sudo kill "$UDPID" 2>/dev/null || true; wait "$UDPID" 2>/dev/null || true

# video0: capture branch
v=/run/schema-udev/data/c81:0
sudo grep -q '^E:ID_V4L_VERSION=2$' "$v" || { echo "FAIL: video0 missing ID_V4L_VERSION"; exit 1; }
sudo grep -q '^E:ID_V4L_PRODUCT=USB2.0 UVC PC Camera: USB2.0 UV$' "$v" || { echo "FAIL: video0 wrong ID_V4L_PRODUCT"; sudo cat "$v"; exit 1; }
sudo grep -q '^E:ID_V4L_CAPABILITIES=:capture:$' "$v" || { echo "FAIL: video0 wrong ID_V4L_CAPABILITIES"; exit 1; }

# video1: empty-caps branch
v1=/run/schema-udev/data/c81:1
sudo grep -q '^E:ID_V4L_CAPABILITIES=:$' "$v1" || { echo "FAIL: video1 ID_V4L_CAPABILITIES not ':'"; sudo cat "$v1"; exit 1; }

# regression: slices 3a/3b intact
sudo grep -q '^E:ID_ATA=1$' /run/schema-udev/data/b8:0 || { echo "FAIL: ATA disk lost ID_ATA"; exit 1; }
sudo grep -q '^E:ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0$' /run/schema-udev/data/b8:48 || { echo "FAIL: usb disk lost composed serial"; exit 1; }

echo ">> RESULT: PASS (v4l_id live gate: 0/0, video0 :capture: + video1 :, 3a/3b intact)"
