#!/bin/sh
# schema-init has no `systemd --user`, so WP inherits no graphical-session env
# and its session-bus modules (dbus, mpris, reserve-device) all fail. The
# session bus is a random per-login /tmp/dbus-XXXX path, so it can't be
# hardcoded. Harvest it (and Wayland/X vars) from a live Plasma/kwin process,
# bounded-waiting for login, then hand it to WP. Degraded fallback (no session
# integration) after the cap so a headless/SSH boot still starts audio.
# PID 1 runs this as the desktop user (user=, limit_* in wireplumber.svc).
UIDN=$(id -u)
harvest() {
    for p in $(pgrep -u "$UIDN" -x plasmashell) \
             $(pgrep -u "$UIDN" -x kwin_wayland) \
             $(pgrep -u "$UIDN" -x kwin_x11); do
        e="/proc/$p/environ"
        [ -r "$e" ] || continue
        for v in DBUS_SESSION_BUS_ADDRESS WAYLAND_DISPLAY DISPLAY XAUTHORITY; do
            line=$(tr '\0' '\n' < "$e" | grep "^$v=" | head -1)
            [ -n "$line" ] && eval "export $line"
        done
        [ -n "$DBUS_SESSION_BUS_ADDRESS" ] && return 0
    done
    return 1
}

i=0
while ! harvest; do
    i=$((i + 1))
    [ "$i" -ge 60 ] && break
    sleep 1
done

export GIO_USE_VFS=local
exec /usr/bin/wireplumber
