#!/bin/sh
# The plasmashell watchdog must judge this session by this user's processes:
# another uid's kwin_wayland (sddm's greeter) kept it alive after logout, and
# another uid's plasmashell stopped it respawning this user's.
# Needs a second uid in a user+pid namespace (newuidmap); /tmp is a private
# tmpfs there, since the watchdog logs to a fixed /tmp path.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
WD="$HERE/distros/fedora-kde/scripts/schema-plasma-watchdog.sh"
unshare -r --map-auto -p -f -m --mount-proc sh -c 'mount -t tmpfs tmpfs /tmp && command -v setpriv' >/dev/null 2>&1 \
    || { echo "SKIP: plasma watchdog (no unprivileged namespace with subordinate uids, tmpfs and setpriv)"; exit 0; }
T=$(mktemp -d -p /var/tmp)
trap '[ -n "${KEEP:-}" ] && echo "kept $T" || rm -rf "$T"' EXIT
chmod 755 "$T"
mkdir -p "$T/bin" "$T/home" "$T/run"
chmod 755 "$T/bin"
printf '#!/bin/sh\nwhile :; do sleep 1; done\n' > "$T/bin/kwin_wayland"
# Copies started by the test carry INITIAL; one the watchdog respawns doesn't.
printf '#!/bin/sh\n[ -n "${INITIAL:-}" ] || touch "%s/respawned.$(id -u)"\nwhile :; do sleep 1; done\n' "$T" > "$T/bin/plasmashell"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/kbuildsycoca6"
chmod 755 "$T/bin/"*

# $1: shell snippet run inside the namespace as uid 0, after /tmp is private
ns() {
    unshare -r --map-auto -p -f -m --mount-proc env -i PATH="$T/bin:$PATH" HOME="$T/home" \
        XDG_RUNTIME_DIR="$T/run" WAYLAND_DISPLAY=wayland-0 T="$T" WD="$WD" SNIP="$1" sh -c '
        # never let the watchdog write the real /tmp log
        mount -t tmpfs tmpfs /tmp || exit 1
        # waits for the copy to be up: one that never started would make the
        # test pass against an unscoped watchdog
        other() {
            setpriv --reuid 1000 --regid 1000 --clear-groups env INITIAL=1 XDG_MENU_PREFIX=plasma- "$@" &
            i=0; until pgrep -U 1000 -x "$1" >/dev/null; do
                [ $i -lt 50 ] || { echo "other user'"'"'s $1 never started" >> "$T/err"; exit 1; }
                sleep 0.1; i=$((i+1))
            done
        }
        mine() { env INITIAL=1 XDG_MENU_PREFIX=plasma- "$@" & }
        eval "$SNIP"
        cp /tmp/schema-plasma-watchdog.log "$T/wd.log" 2>/dev/null || true
    '
}

setup_fail() { echo "FAIL: $(cat "$T/err" 2>/dev/null || echo "namespace setup failed")"; exit 1; }
fail=0
ns '
    other kwin_wayland
    mine plasmashell
    sleep 0.5
    rc=0; timeout 15 "$WD" || rc=$?
    echo "$rc" > "$T/rc"
' || setup_fail
[ "$(cat "$T/rc")" = 0 ] && grep -q "compositor gone" "$T/wd.log" \
    || { echo "FAIL: another user's kwin_wayland kept the watchdog running (rc=$(cat "$T/rc"))"; fail=1; }

ns '
    mine kwin_wayland
    other plasmashell
    mine plasmashell
    sleep 0.5
    "$WD" &
    sleep 2
    pkill -U 0 -x plasmashell
    i=0; while [ ! -e "$T/respawned.0" ] && [ $i -lt 60 ]; do sleep 0.2; i=$((i+1)); done
    pgrep -U 1000 -x plasmashell >/dev/null && : > "$T/other-alive"
' || setup_fail
[ -e "$T/respawned.0" ] \
    || { echo "FAIL: another user's plasmashell stopped the watchdog respawning this user's"; fail=1; }
[ -e "$T/other-alive" ] || { echo "FAIL: the other user's plasmashell did not survive"; fail=1; }

[ $fail -eq 0 ] && echo "PASS: plasma watchdog follows only its own user's kwin_wayland and plasmashell"
exit $fail
