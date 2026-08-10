#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev

sudo rm -rf /run/schema-udev
sudo ./schema-udev & UDPID=$!
sleep 3
sudo kill "$UDPID" 2>/dev/null || true
wait "$UDPID" 2>/dev/null || true

UADIR=/run/schema-udev/uaccess
fail=0

# SECURITY-CRITICAL (forward / only-ours): every shadow decision must match a
# real ACL logind applied. A shadow grant with no matching logind ACL = FAIL.
n_fwd=0
for rec in "$UADIR"/*; do
    [ -e "$rec" ] || continue
    n_fwd=$((n_fwd + 1))
    node=$(sed -n 's/^DEVNODE=//p' "$rec")
    uid=$(sed -n 's/^GRANT_UID=//p' "$rec")
    if ! getfacl -n -p "$node" 2>/dev/null | grep -q "^user:$uid:rw-"; then
        echo "FAIL security: shadow-granted $node (uid $uid) has NO matching logind ACL"
        fail=1
    fi
done
echo "forward/only-ours: $n_fwd shadow decisions, all matched logind ACL"

# COMPLETENESS (reverse): every in-scope real uaccess node has a shadow record.
# In-scope subsystems only: sound, video4linux, media. Deferred (excluded):
# dri, usb, hidraw, rfkill, udmabuf, optical.
aid=$(sed -n 's/^ACTIVE_UID=//p' /run/systemd/seats/seat0)
n_rev=0
for node in /dev/snd/* /dev/video* /dev/media*; do
    [ -c "$node" ] || continue
    getfacl -n -p "$node" 2>/dev/null | grep -q "^user:$aid:rw-" || continue  # logind didn't grant
    maj=$((0x$(stat -c%t "$node"))); min=$((0x$(stat -c%T "$node")))
    n_rev=$((n_rev + 1))
    if [ ! -e "$UADIR/c$maj:$min" ]; then
        echo "FAIL completeness: $node (c$maj:$min) has logind ACL but NO shadow record"
        fail=1
    fi
done
echo "reverse: $n_rev in-scope logind-granted nodes, all have shadow records"

[ "$fail" = 0 ] || { echo ">> RESULT: FAIL"; exit 1; }
echo ">> RESULT: PASS (dry-run uaccess decisions == logind ACLs; sound/video4linux/media in-scope; dri/usb/hidraw/rfkill/udmabuf/optical deferred)"
