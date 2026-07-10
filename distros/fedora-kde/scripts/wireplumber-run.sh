#!/bin/sh
[ -r /etc/schema-init/user.conf ] && . /etc/schema-init/user.conf
SCHEMA_USER="${SCHEMA_USER:-ajax80}"
SCHEMA_UID="${SCHEMA_UID:-1000}"

# schema-init has no `systemd --user`, so WP inherits no graphical-session env
# and its session-bus modules (dbus, mpris, reserve-device) all fail. The
# session bus is a random per-login /tmp/dbus-XXXX path, so it can't be
# hardcoded. Harvest it (and Wayland/X vars) from a live Plasma/kwin process,
# bounded-waiting for login, then hand it to WP. Degraded fallback (no session
# integration) after the cap so a headless/SSH boot still starts audio.
harvest() {
    for p in $(pgrep -u "$SCHEMA_UID" -x plasmashell) \
             $(pgrep -u "$SCHEMA_UID" -x kwin_wayland) \
             $(pgrep -u "$SCHEMA_UID" -x kwin_x11); do
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

exec runuser -u "$SCHEMA_USER" -- env \
    XDG_RUNTIME_DIR="/run/user/$SCHEMA_UID" \
    GIO_USE_VFS=local \
    ${DBUS_SESSION_BUS_ADDRESS:+DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"} \
    ${WAYLAND_DISPLAY:+WAYLAND_DISPLAY="$WAYLAND_DISPLAY"} \
    ${DISPLAY:+DISPLAY="$DISPLAY"} \
    ${XAUTHORITY:+XAUTHORITY="$XAUTHORITY"} \
    /usr/bin/wireplumber
