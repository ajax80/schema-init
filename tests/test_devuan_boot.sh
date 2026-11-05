#!/bin/sh
# LightDM keys its X cookie on the hostname when it starts, and the greeter looks
# it up by the hostname then. Set late, the greeter dies with "Authorization required".
set -eu
d=distros/devuan
fail() { echo "FAIL: $1"; exit 1; }

grep -q "/proc/sys/kernel/hostname" "$d/scripts/devuan-boot" || fail "devuan-boot does not set the hostname"

# Every display manager must be ordered after devuan-boot, through its deps.
reaches_boot() {
    [ "$1" = devuan-boot ] && return 0
    for dep in $(sed -n "s/^dep=//p" "$d/services/$1.svc" 2>/dev/null); do
        reaches_boot "$dep" && return 0
    done
    return 1
}
for dm in lightdm slim; do
    reaches_boot "$dm" || fail "$dm can start before devuan-boot"
done
echo "ok: devuan hostname is set before the display manager"
