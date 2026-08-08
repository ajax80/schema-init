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

# sr0 optical: capability keys reproduced (anti-hollow)
c=/run/schema-udev/data/b11:0
[ -e "$c" ] || { echo "FAIL: no shadow record for sr0 b11:0"; exit 1; }
sudo grep -q '^E:ID_CDROM=1$' "$c" || { echo "FAIL: sr0 missing ID_CDROM"; sudo cat "$c"; exit 1; }
sudo grep -q '^E:ID_CDROM_DVD_RAM=1$' "$c" || { echo "FAIL: sr0 missing ID_CDROM_DVD_RAM"; exit 1; }
sudo grep -q '^E:ID_CDROM_CD_RW=1$' "$c" || { echo "FAIL: sr0 missing ID_CDROM_CD_RW"; exit 1; }
sudo grep -q '^E:ID_CDROM_RW_REMOVABLE=1$' "$c" || { echo "FAIL: sr0 missing ID_CDROM_RW_REMOVABLE"; exit 1; }

# regression: slices 3a/3b/3c intact
sudo grep -q '^E:ID_ATA=1$' /run/schema-udev/data/b8:0 || { echo "FAIL: ATA disk lost ID_ATA"; exit 1; }
sudo grep -q '^E:ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0$' /run/schema-udev/data/b8:48 || { echo "FAIL: usb disk lost composed serial"; exit 1; }
sudo grep -q '^E:ID_V4L_CAPABILITIES=:capture:$' /run/schema-udev/data/c81:0 || { echo "FAIL: video0 lost v4l caps"; exit 1; }

echo ">> RESULT: PASS (cdrom_id live gate: 0/0, sr0 capabilities reproduced, 3a/3b/3c intact)"
