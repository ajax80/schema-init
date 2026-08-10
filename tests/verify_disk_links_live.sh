#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev

sudo rm -rf /run/schema-udev
sudo rm -rf /dev/schema/disk
sudo ./schema-udev & UDPID=$!
sleep 3
sudo kill "$UDPID" 2>/dev/null || true
wait "$UDPID" 2>/dev/null || true

fail=0
for tree in by-uuid by-label by-partuuid by-partlabel by-path by-diskseq; do
    real="/dev/disk/$tree"
    ours="/dev/schema/disk/$tree"
    [ -d "$real" ] || { echo "skip $tree (udevd has none)"; continue; }
    rn=$(find "$real" -maxdepth 1 -mindepth 1 -type l -printf '%f\n' 2>/dev/null | sort)
    if [ "$tree" = "by-path" ]; then
        rn=$(printf '%s\n' "$rn" | grep -v -E '\-ata-[0-9]+($|-part)|-usbv[0-9]+')
    fi
    on=$(find "$ours" -maxdepth 1 -mindepth 1 -type l -printf '%f\n' 2>/dev/null | sort)
    if [ "$rn" != "$on" ]; then
        echo "FAIL $tree: name-set differs"
        echo "  only-udevd:"; comm -23 <(printf '%s\n' "$rn") <(printf '%s\n' "$on") | sed 's/^/    /'
        echo "  only-ours:";  comm -13 <(printf '%s\n' "$rn") <(printf '%s\n' "$on") | sed 's/^/    /'
        fail=1; continue
    fi
    for nm in $on; do
        rd=$(realpath "$real/$nm" 2>/dev/null || true)
        od=$(realpath "$ours/$nm" 2>/dev/null || true)
        [ "$rd" = "$od" ] || { echo "FAIL $tree/$nm: resolves '$od' != '$rd'"; fail=1; }
    done
    echo "OK $tree ($(printf '%s\n' "$on" | grep -c .) links)"
done

[ "$fail" = "0" ] || { echo ">> RESULT: FAIL"; exit 1; }
echo ">> RESULT: PASS (six in-scope by-* trees match udevd set-wise + resolved device; by-id excluded)"
