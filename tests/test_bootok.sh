#!/bin/sh
# schema-bootok marks the boot good only when a login user's kwin_wayland holds
# a DRM card: a greeter's compositor (system uid) must not count.
# Needs a user+pid+mount namespace with subordinate uids; /dev/dri is replaced
# there by a tmpfs holding a plain file named card0.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
BOOTOK="$HERE/scripts/schema-bootok"
unshare -r --map-auto -p -f -m --mount-proc sh -c \
    '[ -d /dev/dri ] && mount -t tmpfs tmpfs /dev/dri && command -v setpriv' >/dev/null 2>&1 \
    || { echo "SKIP: schema-bootok (no namespace with subordinate uids, /dev/dri and setpriv)"; exit 0; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
chmod 755 "$T"
mkdir -p "$T/bin"
chmod 755 "$T/bin"
printf '#!/bin/sh\nexec 3</dev/dri/card0\nwhile :; do sleep 1; done\n' > "$T/bin/kwin_wayland"
printf '#!/bin/sh\necho "$@" >> "%s/editenv"\n' "$T" > "$T/bin/grub2-editenv"
chmod 755 "$T/bin/"*

# $1: uid that runs kwin_wayland. Writes bootok's exit code to $T/rc.
run() {
    rm -f "$T/editenv" "$T/rc"
    unshare -r --map-auto -p -f -m --mount-proc env -i PATH="$T/bin:$PATH" T="$T" BOOTOK="$BOOTOK" AS="$1" \
        SCHEMA_BOOTOK_TIMEOUT=4 SCHEMA_GRUBENV="$T/grubenv" sh -c '
        mount -t tmpfs tmpfs /dev/dri || exit 1
        : > /dev/dri/card0
        chmod 644 /dev/dri/card0
        setpriv --reuid "$AS" --regid "$AS" --clear-groups kwin_wayland &
        i=0; until pgrep -U "$AS" -x kwin_wayland >/dev/null; do
            [ $i -lt 50 ] || exit 1; sleep 0.1; i=$((i+1))
        done
        rc=0; "$BOOTOK" >/dev/null 2>&1 || rc=$?
        echo "$rc" > "$T/rc"
    ' || { echo "FAIL: namespace setup (uid $1)"; exit 1; }
}

fail=0
run 1000
[ "$(cat "$T/rc")" = 0 ] && grep -q "set boot_success=1" "$T/editenv" 2>/dev/null \
    || { echo "FAIL: a login user's compositor did not mark the boot good"; fail=1; }
run 990
[ "$(cat "$T/rc")" = 1 ] && [ ! -e "$T/editenv" ] \
    || { echo "FAIL: a system uid's (greeter's) compositor marked the boot good"; fail=1; }

[ $fail -eq 0 ] && echo "PASS: schema-bootok counts only a login user's compositor"
exit $fail
