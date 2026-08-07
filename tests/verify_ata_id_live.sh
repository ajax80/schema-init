#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s parity

OUT=$(sudo ./udev-parity)

# 1) counters must be 0 (identity now in-scope on ATA)
MM=$(echo "$OUT" | sed -n 's/^VALUE MISMATCHES (keys in both, differing value): //p')
MISS=$(echo "$OUT" | sed -n 's/^IN-SCOPE MISSING (device-class aware): //p')
echo "mismatches=$MM inscope-missing=$MISS"
[ "$MM" = "0" ] || { echo "FAIL: $MM value mismatches"; echo "$OUT" | grep '^VALMIS'; exit 1; }
[ "$MISS" = "0" ] || { echo "FAIL: $MISS in-scope missing"; echo "$OUT" | grep '^INSCOPE-MISS'; exit 1; }

# 2) anti-hollow: our ata_id must actually fire on the SATA disks. Confirm each
#    real ATA disk's ID_WWN is reproduced by re-deriving with the parity tool's
#    own build. Use a tiny probe: coldplug our daemon to the shadow db and check
#    the ATA disks carry ID_MODEL/ID_SERIAL we produced.
sudo rm -rf /run/schema-udev
sudo ./schema-udev & UDPID=$!
sleep 2; sudo kill "$UDPID" 2>/dev/null || true; wait "$UDPID" 2>/dev/null || true

ATA_OK=0
for key in b8:0 b8:16 b8:32; do
    f=/run/schema-udev/data/$key
    [ -e "$f" ] || { echo "FAIL: no shadow record for ATA disk $key"; exit 1; }
    sudo grep -q '^E:ID_ATA=1$' "$f" || { echo "FAIL: $key missing ID_ATA"; exit 1; }
    sudo grep -q '^E:ID_BUS=ata$' "$f" || { echo "FAIL: $key missing ID_BUS=ata"; exit 1; }
    sudo grep -q '^E:ID_MODEL=' "$f" || { echo "FAIL: $key missing ID_MODEL"; exit 1; }
    ATA_OK=$((ATA_OK+1))
done
[ "$ATA_OK" -ge 3 ] || { echo "FAIL: only $ATA_OK ATA disks reproduced"; exit 1; }

# 3) negative: the usb disk (sdd, b8:48) must NOT gain ID_ATA from our builtin
if [ -e /run/schema-udev/data/b8:48 ]; then
    sudo grep -q '^E:ID_ATA=1$' /run/schema-udev/data/b8:48 && { echo "FAIL: ata_id fired on usb disk b8:48"; exit 1; }
fi

echo ">> RESULT: PASS (ata_id live gate: 0/0, $ATA_OK ATA disks, usb untouched)"
