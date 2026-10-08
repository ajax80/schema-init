#!/bin/bash
# schema-plasma-autologin.sh — the canonical GUI-login path for every
# schema-init desktop profile (fedora-kde, fedora-installer). Autologs the
# machine's primary human user straight into a Plasma Wayland session under
# schema-init as PID 1. No display-manager greeter — the box was set up for one
# person, so it boots to their desktop.
#
# bash, not sh, for $BASHPID: the process that joins the session-scope cgroup
# must be the one that execs the compositor.
exec >> /var/log/schema-autologin.log 2>&1
# ready_path for this service: kwin's wayland socket appearing means the
# compositor is up. A fixed marker keeps the .svc independent of the uid.
READY=/run/schema-plasma-ready
rm -f "$READY"
set -x

# --- who logs in. user.conf if the installer wrote one; else auto-detect the
#     first real human account (uid 1000..64999). Never fail out over this.
[ -r /etc/schema-init/user.conf ] && . /etc/schema-init/user.conf
if [ -z "${SCHEMA_USER:-}" ]; then
    SCHEMA_USER=$(awk -F: '$3>=1000 && $3<65000 {print $1; exit}' /etc/passwd)
fi
SCHEMA_USER="${SCHEMA_USER:-user}"
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

# Without drm in the initramfs the real GPU driver loads after switch-root, and
# on a fast box the session can get here first and start kwin on the firmware
# framebuffer (simpledrm binds as "simple-framebuffer"). Wait until a card has a
# real driver and no firmware card is left: on a hybrid laptop the discrete GPU
# can bind before the one that owns the boot display. Carry on after 30s so a
# machine with only a firmware framebuffer still gets its desktop.
( set +x
  for _ in $(seq 1 300); do
      real="" fw=""
      for c in /sys/class/drm/card*; do
          case "${c##*/}" in *-*) continue ;; esac
          d=$(readlink "$c/device/driver" 2>/dev/null) || continue
          case "${d##*/}" in
              simple-framebuffer|efi-framebuffer|vesa-framebuffer|simpledrm|efidrm|vesadrm) fw=1 ;;
              *) real="${c##*/} ${d##*/}" ;;
          esac
      done
      [ -n "$real" ] && [ -z "$fw" ] && { echo "gpu_ready $real"; exit 0; }
      sleep 0.1
  done
  echo "gpu_wait_timeout" )

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
# A respawn after the user switched consoles must take its VT back: kwin draws
# without it, but the kernel keeps feeding keys to the active VT's tty.
timeout 5 chvt "$SCHEMA_VTNR" 2>/dev/null || true

REGISTER=/usr/local/bin/schema-session-register
UNREGISTER=/usr/local/bin/schema-session-unregister

SID=""
release_session() {
    [ -n "$SID" ] || return 0
    [ -x "$UNREGISTER" ] && "$UNREGISTER" "$SID" "$SCHEMA_UID" 2>/dev/null || true
    SID=""
}
# The session runs in its logind scope, outside this service's cgroup, so
# PID 1's stop signal reaches only this script. Pass it on to everything in
# the scope (kwin, plasmashell, the user's apps), give them time to save and
# exit inside this service's stop budget (20 s with stop_first, via the
# -session drop-in: a kill while plasmashell is still writing a first-login
# panel layout leaves a half-built one it never rebuilds), then release the
# seat and go.
SESSION_SCOPE=""
SESSION_PID=""
# Everything in the scope except the session's plumbing: the compositor, the
# session bus and the scripts that hold them (plasma-session-start's TERM trap
# takes kwin down with it).
session_apps() {
    for p in $(cat "$SESSION_SCOPE/cgroup.procs" 2>/dev/null); do
        case "$(cat "/proc/$p/comm" 2>/dev/null)" in
            kwin_wayland|kwin_wayland_wr|Xwayland|schema-dbus|schema-dbus-ses|dbus-daemon|dbus-broker*|plasma-session-|runuser|sleep|"") ;;
            *) printf '%s ' "$p" ;;
        esac
    done
}
stop_session() {
    if [ -n "$SESSION_SCOPE" ] && [ -r "$SESSION_SCOPE/cgroup.procs" ]; then
        # Shell, apps and helpers first, while the compositor and the session
        # bus they talk to on the way out are still up (systemd's order);
        # then the plumbing.
        # Wait for the ones signalled, not for an empty scope: services the
        # exiting apps call on the way out get bus-activated anew, and those
        # go with the plumbing.
        # Paced to fit this service's stop budget (SCHEMA_STOP_TIMEOUT_SEC from
        # PID 1, else its 3 s default): 0.5 s margin, 2 s for the plumbing,
        # the rest for the apps.
        # kded6 drops a TERM that lands before its event loop runs (its handler
        # only calls qApp->quit()), and a first-login kded6 spends seconds
        # loading modules: TERM it again every 0.5 s while it is in the scope.
        tenths=$(( ${SCHEMA_STOP_TIMEOUT_SEC:-3} * 10 - 25 ))
        apps=$(session_apps)
        [ -n "$apps" ] && kill -TERM $apps 2>/dev/null
        for t in $(seq 1 "$tenths"); do
            alive=""
            for p in $apps; do kill -0 "$p" 2>/dev/null && { alive=1; break; }; done
            [ -n "$alive" ] || break
            [ $((t % 5)) -eq 0 ] && for p in $(cat "$SESSION_SCOPE/cgroup.procs" 2>/dev/null); do
                [ "$(cat "/proc/$p/comm" 2>/dev/null)" = kded6 ] && kill -TERM "$p" 2>/dev/null
            done
            sleep 0.1
        done
        kill -TERM $(cat "$SESSION_SCOPE/cgroup.procs") 2>/dev/null
        for _ in $(seq 1 20); do
            [ -n "$(cat "$SESSION_SCOPE/cgroup.procs" 2>/dev/null)" ] || break
            sleep 0.1
        done
        # Anything left would hold the seat and DRM against the respawned
        # session (schema-ctl restart), or linger after a stop.
        if [ -n "$(cat "$SESSION_SCOPE/cgroup.procs" 2>/dev/null)" ]; then
            printf 'session_kill\n'
            echo 1 > "$SESSION_SCOPE/cgroup.kill" 2>/dev/null ||
                kill -KILL $(cat "$SESSION_SCOPE/cgroup.procs") 2>/dev/null
            for _ in $(seq 1 10); do
                [ -n "$(cat "$SESSION_SCOPE/cgroup.procs" 2>/dev/null)" ] || break
                sleep 0.05
            done
        fi
    elif [ -n "$SESSION_PID" ]; then
        kill -TERM "$SESSION_PID" 2>/dev/null
    fi
    printf 'session_stopped\n'
    release_session
    exit 0
}
trap 'release_session' EXIT
trap 'stop_session' HUP INT TERM

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

    [ -e "$READY" ] || ( set +x
        for _ in $(seq 1 600); do
            [ -S "/run/user/$SCHEMA_UID/wayland-0" ] && { : > "$READY"; break; }
            sleep 0.2
        done ) &

    # A background job starts with SIGINT/SIGQUIT ignored; env puts them back
    # so the session does not inherit that.
    ( echo $BASHPID > "$SESSION_SCOPE/cgroup.procs" 2>/dev/null || true
      exec env --default-signal=INT,QUIT runuser -u "$SCHEMA_USER" -- env \
        HOME="$SCHEMA_HOME" \
        USER="$SCHEMA_USER" \
        LOGNAME="$SCHEMA_USER" \
        SHELL="$SCHEMA_SHELL" \
        XDG_RUNTIME_DIR="/run/user/$SCHEMA_UID" \
        XDG_DATA_DIRS="$SCHEMA_DATA_DIRS" \
        XDG_CONFIG_DIRS="/etc/xdg:/usr/share/kde-settings/kde-profile/default/xdg" \
        XDG_MENU_PREFIX="plasma-" \
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
        /usr/local/bin/schema-dbus-session-run.sh /usr/local/bin/plasma-session-start.sh ) &
    SESSION_PID=$!
    wait "$SESSION_PID"
    RC=$?
    SESSION_PID=""
    release_session
    printf 'plasma_exited rc=%d\n' $RC
    [ $RC -ne 0 ] && break
    sleep 2
done
