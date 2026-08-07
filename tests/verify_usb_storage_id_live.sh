#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev parity

# counters via the parity tool (sudo: blkid raw reads)
OUT=$(sudo ./udev-parity)
MM=$(echo "$OUT" | sed -n 's/^VALUE MISMATCHES (keys in both, differing value): //p')
MISS=$(echo "$OUT" | sed -n 's/^IN-SCOPE MISSING (device-class aware): //p')
echo "mismatches=$MM inscope-missing=$MISS"
[ "$MM" = "0" ] || { echo "FAIL: $MM value mismatches"; echo "$OUT" | grep '^VALMIS'; exit 1; }
[ "$MISS" = "0" ] || { echo "FAIL: $MISS in-scope missing"; echo "$OUT" | grep '^INSCOPE-MISS'; exit 1; }

# coldplug into the shadow db and check the usb disk + its partition
sudo rm -rf /run/schema-udev
sudo ./schema-udev & UDPID=$!
sleep 2; sudo kill "$UDPID" 2>/dev/null || true; wait "$UDPID" 2>/dev/null || true

d=/run/schema-udev/data/b8:48
sudo grep -q '^E:ID_BUS=usb$' "$d" || { echo "FAIL: sdd missing ID_BUS=usb"; exit 1; }
sudo grep -q '^E:ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0$' "$d" || { echo "FAIL: sdd wrong/missing composed ID_SERIAL"; sudo cat "$d"; exit 1; }
sudo grep -q '^E:ID_INSTANCE=0:0$' "$d" || { echo "FAIL: sdd missing ID_INSTANCE=0:0"; exit 1; }

part=/run/schema-udev/data/b8:49
if [ -e "$part" ]; then
    sudo grep -q '^E:ID_BUS=usb$' "$part" || { echo "FAIL: sdd1 did not inherit ID_BUS=usb"; exit 1; }
    sudo grep -q '^E:ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0$' "$part" || { echo "FAIL: sdd1 did not inherit ID_SERIAL"; exit 1; }
fi

# regression: ATA disks still native
for k in b8:0 b8:16 b8:32; do
    sudo grep -q '^E:ID_ATA=1$' "/run/schema-udev/data/$k" || { echo "FAIL: $k lost ID_ATA"; exit 1; }
    sudo grep -q '^E:ID_BUS=ata$' "/run/schema-udev/data/$k" || { echo "FAIL: $k lost ID_BUS=ata"; exit 1; }
done

echo ">> RESULT: PASS (usb-storage id live gate: 0/0, sdd+sdd1 composed serial, ATA intact)"
