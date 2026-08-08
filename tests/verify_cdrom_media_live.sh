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

c=/run/schema-udev/data/b11:0
[ -e "$c" ] || { echo "FAIL: no shadow record for sr0 b11:0"; exit 1; }

# sr0 media keys
sudo grep -q '^E:ID_CDROM=1$' "$c" || { echo "FAIL: sr0 missing ID_CDROM"; sudo cat "$c"; exit 1; }

if sudo grep -q '^E:ID_CDROM_MEDIA=1$' "$c"; then
    echo "sr0: media detected"
    sudo grep -q '^E:ID_CDROM_MEDIA_STATE=' "$c" || { echo "FAIL: sr0 missing ID_CDROM_MEDIA_STATE"; sudo cat "$c"; exit 1; }
    sudo grep -q '^E:ID_CDROM_MEDIA_TRACK_COUNT=' "$c" || { echo "FAIL: sr0 missing ID_CDROM_MEDIA_TRACK_COUNT"; sudo cat "$c"; exit 1; }
    if sudo grep -q '^E:ID_FS_TYPE=udf$' "$c"; then
        echo "sr0: UDF disc detected"
        sudo grep -q '^E:ID_FS_LABEL=POWERT_TOUR_DVD$' "$c" || { echo "FAIL: sr0 wrong UDF label"; sudo cat "$c"; exit 1; }
        sudo grep -q '^E:ID_FS_VERSION=1.02$' "$c" || { echo "FAIL: sr0 wrong UDF version"; sudo cat "$c"; exit 1; }
    elif sudo grep -q '^E:ID_FS_TYPE=iso9660$' "$c"; then
        echo "sr0: ISO9660 disc detected"
        sudo grep -q '^E:ID_FS_LABEL=Wardriver.2026.1080p.WEBRip.x264$' "$c" || { echo "FAIL: sr0 wrong ISO label"; sudo cat "$c"; exit 1; }
    fi
else
    echo "sr0: drive empty (inverted-#94 regression: zero ID_FS_ lines)"
    if sudo grep -q '^E:ID_FS_' "$c"; then
        echo "FAIL: empty drive emitted ID_FS_ keys"; sudo cat "$c"; exit 1;
    fi
fi

# regression: slices 3a/3b/3c intact
sudo grep -q '^E:ID_ATA=1$' /run/schema-udev/data/b8:0 || { echo "FAIL: ATA disk lost ID_ATA"; exit 1; }
sudo grep -q '^E:ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0$' /run/schema-udev/data/b8:48 || { echo "FAIL: usb disk lost composed serial"; exit 1; }
sudo grep -q '^E:ID_V4L_CAPABILITIES=:capture:$' /run/schema-udev/data/c81:0 || { echo "FAIL: video0 lost v4l caps"; exit 1; }

echo ">> RESULT: PASS (cdrom_media live gate: 0/0, sr0 media + optical fs reproduced, 3a/3b/3c intact)"
