#!/bin/sh
# LightDM keys its X cookie on the hostname when it starts, and the greeter looks
# it up by the hostname then. Set late, the greeter dies with "Authorization required".
set -eu
cd "$(dirname "$0")/.."
d=distros/devuan
fail() { echo "FAIL: $1"; exit 1; }

grep -Eq "^[^#]*> */proc/sys/kernel/hostname" "$d/scripts/devuan-boot" || fail "devuan-boot does not set the hostname"

for dm in lightdm slim; do
    grep -qx "dep=devuan-boot" "$d/services/$dm.svc" || fail "$dm can start before devuan-boot"
done
echo "ok: devuan hostname is set before the display manager"
