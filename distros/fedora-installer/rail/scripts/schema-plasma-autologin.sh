#!/bin/bash
# schema-plasma-autologin.sh — the login path for the dad-proof installer.
# Generalized from blakbox's distros/fedora-kde sddm-logged: autologs the
# machine's primary human user straight into a Plasma Wayland session under
# schema-init as PID 1. No display-manager greeter — the box was set up for one
# person, so it boots to their desktop.
#
# bash, not sh, for $BASHPID: the process that joins the session-scope cgroup
# must be the one that execs the compositor.
exec >> /var/log/schema-autologin.log 2>&1
set -x

# --- who logs in. user.conf if the installer wrote one; else auto-detect the
#     first real human account (uid 1000..64999). Never fail out over this.
[ -r /etc/schema-init/user.conf ] && . /etc/schema-init/user.conf
if [ -z "${SCHEMA_USER:-}" ]; then
    SCHEMA_USER=$(awk -F: '$3>=1000 && $3<65000 {print $1; exit}' /etc/passwd)
fi
SCHEMA_USER="${SCHEMA_USER:-ajax80}"
SCHEMA_UID="${SCHEMA_UID:-$(id -u "$SCHEMA_USER" 2>/dev/null)}"
SCHEMA_UID="${SCHEMA_UID:-1000}"
SCHEMA_SEAT="${SCHEMA_SEAT:-seat0}"
SCHEMA_VTNR="${SCHEMA_VTNR:-1}"
SCHEMA_HOME="${SCHEMA_HOME:-$(getent passwd "$SCHEMA_USER" | cut -d: -f6)}"
SCHEMA_HOME="${SCHEMA_HOME:-/home/$SCHEMA_USER}"
SCHEMA_SHELL="${SCHEMA_SHELL:-$(getent passwd "$SCHEMA_USER" | cut -d: -f7)}"
SCHEMA_SHELL="${SCHEMA_SHELL:-/bin/bash}"
SCHEMA_DATA_DIRS="${SCHEMA_DATA_DIRS:-$SCHEMA_HOME/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:/usr/local/share:/usr/share:/var/lib/snapd/desktop}"

mkdir -p "/run/user/$SCHEMA_UID"
chown "$SCHEMA_UID:$SCHEMA_UID" "/run/user/$SCHEMA_UID"
chmod 700 "/run/user/$SCHEMA_UID"

# input devices coldplugged so libinput sees the keyboard/mouse
udevadm trigger --subsystem-match=input --action=add 2>/dev/null || true
udevadm settle --timeout=10 2>/dev/null || true
stty -F "/dev/tty$SCHEMA_VTNR" -echo 2>/dev/null || true
clear > "/dev/tty$SCHEMA_VTNR" 2>/dev/null || true

# Hand the DRM master from plymouth to the compositor. plymouthd is started in
# the initramfs and persists across switch-root holding /dev/dri; with no
# systemd there is no plymouth-quit.service to release it, so the splash would
# otherwise sit forever and kwin could never open the card (the classic
# first-boot spinner hang). Quit it here, right before the session starts.
# --retain-splash leaves the last frame up until kwin draws, so boot looks
# seamless (splash -> desktop). Then wait for plymouthd to actually exit and
# drop the master before the compositor grabs it, to avoid a DRM race.
if command -v plymouth >/dev/null 2>&1; then
    plymouth quit --retain-splash 2>/dev/null || true
    for _ in $(seq 1 50); do
        pgrep -x plymouthd >/dev/null 2>&1 || break
        sleep 0.1
    done
fi

REGISTER=/usr/local/bin/schema-session-register
UNREGISTER=/usr/local/bin/schema-session-unregister

SID=""
release_session() {
    [ -n "$SID" ] || return 0
    [ -x "$UNREGISTER" ] && "$UNREGISTER" "$SID" "$SCHEMA_UID" 2>/dev/null || true
    SID=""
}
trap 'release_session' EXIT HUP INT TERM

while true; do
    rm -f "/run/user/$SCHEMA_UID"/wayland-* /tmp/.ICE-unix/* /tmp/.X*-lock 2>/dev/null || true

    SID=""
    if [ -x "$REGISTER" ]; then
        SID=$("$REGISTER" --uid "$SCHEMA_UID" --user "$SCHEMA_USER" \
                          --seat "$SCHEMA_SEAT" --vtnr "$SCHEMA_VTNR" \
                          --type wayland --class user --desktop KDE \
                          --display --service schema-autologin --leader $$ 2>/dev/null)
    fi
    [ -n "$SID" ] || SID=31
    SESSION_SCOPE="/sys/fs/cgroup/user.slice/user-$SCHEMA_UID.slice/session-$SID.scope"
    mkdir -p "$SESSION_SCOPE" 2>/dev/null || true

    ( echo $BASHPID > "$SESSION_SCOPE/cgroup.procs" 2>/dev/null || true
      exec runuser -u "$SCHEMA_USER" -- env \
        HOME="$SCHEMA_HOME" \
        USER="$SCHEMA_USER" \
        LOGNAME="$SCHEMA_USER" \
        SHELL="$SCHEMA_SHELL" \
        XDG_RUNTIME_DIR="/run/user/$SCHEMA_UID" \
        XDG_DATA_DIRS="$SCHEMA_DATA_DIRS" \
        XDG_SESSION_ID="$SID" \
        LANG=en_US.UTF-8 \
        PLASMA_USE_SYSTEMD_SCOPE=0 \
        XDG_CURRENT_DESKTOP=KDE \
        XDG_SESSION_TYPE=wayland \
        XDG_SESSION_CLASS=user \
        XDG_SESSION_DESKTOP=KDE \
        XDG_SEAT="$SCHEMA_SEAT" \
        XDG_VTNR="$SCHEMA_VTNR" \
        DESKTOP_SESSION=plasma \
        KDE_FULL_SESSION=true \
        KDE_SESSION_VERSION=6 \
        KDE_SESSION_UID="$SCHEMA_UID" \
        /usr/libexec/plasma-dbus-run-session-if-needed /usr/local/bin/plasma-session-start.sh )
    RC=$?
    release_session
    printf 'plasma_exited rc=%d\n' $RC
    [ $RC -ne 0 ] && break
    sleep 2
done
