#!/bin/sh
# schema-bootok marks the boot good only when a login user's session is up: a
# Wayland compositor of theirs holding a DRM card, or an Xorg holding one beside
# their X11 session shell. A greeter's (system uid) must not count.
# Needs a user+pid+mount namespace with subordinate uids; /dev/dri is replaced
# there by a tmpfs holding a plain file named card0.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
BOOTOK="$HERE/scripts/schema-bootok"
unshare -r --map-auto -p -f -m --mount-proc sh -c \
    '[ -d /dev/dri ] && mount -t tmpfs tmpfs /dev/dri && command -v setpriv' >/dev/null 2>&1 \
    || { echo "SKIP: schema-bootok (no namespace with subordinate uids, /dev/dri and setpriv)"; exit 0; }
T=$(mktemp -d -p /var/tmp)
trap 'rm -rf "$T"' EXIT
chmod 755 "$T"
mkdir -p "$T/bin"
chmod 755 "$T/bin"
# CARD=1: hold the card open, as a compositor or Xorg does
for n in kwin_wayland gnome-shell cinnamon Xorg lightdm-greeter; do
    printf '#!/bin/sh\n[ "$CARD" = 1 ] && exec 3</dev/dri/card0\nwhile :; do sleep 1; done\n' > "$T/bin/$n"
done
printf '#!/bin/sh\necho "$@" >> "%s/editenv"\n' "$T" > "$T/bin/grub2-editenv"
# pinned, so the host's login.defs can't move 990/1000 across the line
printf 'UID_MIN 1000\nUID_MIN 500\nUID_MAX 60000\n' > "$T/login.defs"
chmod 755 "$T/bin/"*

# $@: name:uid:card for each process, started in order. Writes bootok's exit
# code to $T/rc.
run() {
    rm -f "$T/editenv" "$T/rc" "$T/err"
    unshare -r --map-auto -p -f -m --mount-proc env -i PATH="$T/bin:$PATH" T="$T" BOOTOK="$BOOTOK" PROCS="$*" \
        SCHEMA_BOOTOK_TIMEOUT=4 SCHEMA_GRUBENV="$T/grubenv" SCHEMA_LOGIN_DEFS="$T/login.defs" sh -c '
        mount -t tmpfs tmpfs /dev/dri || exit 1
        : > /dev/dri/card0
        chmod 644 /dev/dri/card0
        for p in $PROCS; do
            n=${p%%:*}; c=${p##*:}; u=${p#*:}; u=${u%:*}
            setpriv --reuid "$u" --regid "$u" --clear-groups env CARD="$c" "$n" &
            # comm is cut to 15 chars
            k=$(printf "%.15s" "$n")
            i=0; until pgrep -U "$u" -x "$k" >/dev/null; do
                [ $i -lt 50 ] || { echo "$n as uid $u never started" > "$T/err"; exit 1; }
                sleep 0.1; i=$((i+1))
            done
        done
        rc=0; "$BOOTOK" >/dev/null 2>&1 || rc=$?
        echo "$rc" > "$T/rc"
    ' || { echo "FAIL: $(cat "$T/err" 2>/dev/null || echo "namespace setup ($*)")"; exit 1; }
}

fail=0
# $1: good|bad, $2: what it means, rest: processes
expect() {
    want=$1 what=$2; shift 2
    run "$@"
    if [ "$want" = good ]; then
        [ "$(cat "$T/rc")" = 0 ] && grep -q "set boot_success=1" "$T/editenv" 2>/dev/null && return
    else
        [ "$(cat "$T/rc")" = 1 ] && [ ! -e "$T/editenv" ] && return
    fi
    echo "FAIL: $what (want $want, rc=$(cat "$T/rc"))"; fail=1
}

expect good "a login user's kwin_wayland"             kwin_wayland:1000:1
expect bad  "a greeter's kwin_wayland (system uid)"   kwin_wayland:990:1
# the greeter's comes first in pgrep's list and must not hide the user's
expect good "greeter's and user's kwin_wayland"       kwin_wayland:990:1 kwin_wayland:1000:1
expect good "a user above UID_MAX (homed, sssd)"      kwin_wayland:60100:1
expect bad  "a user's kwin_wayland with no card"      kwin_wayland:1000:0
expect good "a login user's GNOME on Wayland"         gnome-shell:1000:1
# Fedora Cinnamon: LightDM, a root Xorg holding the card, cinnamon as the user
expect good "root Xorg + the user's cinnamon"         Xorg:0:1 cinnamon:1000:0
expect bad  "root Xorg + LightDM's greeter only"      Xorg:0:1 lightdm-greeter:990:0
expect bad  "root Xorg + a system uid's cinnamon"     Xorg:0:1 cinnamon:990:0
expect bad  "the user's cinnamon with no Xorg"        cinnamon:1000:0

[ $fail -eq 0 ] && echo "PASS: schema-bootok counts only a login user's session (Wayland or X11)"
exit $fail
