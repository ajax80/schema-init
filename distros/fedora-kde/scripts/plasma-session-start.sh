#!/bin/sh
# schema-init GUI session launcher — DIRECT-LAUNCH model.
#
# 2026-09-16 (Claire): replaced `startplasma-wayland` with a direct
# kwin_wayland + plasmashell launch. startplasma-wayland's bring-up depends on
# the reclaimed session-bus org.freedesktop.systemd1 shim (StartUnit cascade)
# and xdg-desktop-portal (impl.portal.desktop.kde) activation ORDERING; under
# schema-init (no `systemd --user`) that cascade intermittently mis-orders and
# kwin never finishes coming up -> black screen forever (~1-in-N boots: the hung
# boot 2026-09-16 10:06 vs the clean 10:07 differed ONLY here — kwin never
# requested org.kde.NightTime, KSplash/portal never completed). Every
# hand-recovery in the 2026-09-15 fight used this direct launch and it never
# once hung, because it bypasses startplasma/ksmserver and the racing dbus
# cascade. Cost: no ksmserver session save/restore; the pieces it used to
# provide (env.d replay, PipeWire, KDE daemons, XDG-autostart) are launched
# explicitly below. See project_schema_logind_seat_devices.md /
# project_schema_dbus_session_portal_stall.md.

# Load system and user profiles (PATH, XDG_DATA_DIRS, etc.)
if [ -f /etc/profile ]; then
    . /etc/profile
fi
if [ -f "$HOME/.profile" ]; then
    . "$HOME/.profile"
elif [ -f "$HOME/.bash_profile" ]; then
    . "$HOME/.bash_profile"
fi

# schema-init has no PAM/login step, so SHELL is otherwise inherited as
# /bin/bash from dbus-run-session; set it from passwd here (the session root) so
# the whole tree inherits it. Guard against empty getent so SHELL is never "".
_sh="$(getent passwd "$(id -un)" | cut -d: -f7)"
[ -n "$_sh" ] && export SHELL="$_sh"

# Replay the session env startplasma-wayland would normally source:
# ~/.config/plasma-workspace/env/*.sh — HOME (00), XDG_MENU_PREFIX/CONFIG_DIRS
# to avoid the ksycoca-rebuild war (05), XAUTHORITY (06), flatpak XDG_DATA_DIRS,
# ssh-agent sock, environment.d(5) replay (zzz), XDG_DATA_DIRS dedup (zzzz) —
# and fire the XDG-autostart runner (zz-schema-autostart.sh ->
# schema-autostart-runner.sh: xauth cookie merge, ssh key, ~/.config/autostart
# sweep, plasmashell watchdog). Sourced in glob (lexical) order, exactly as
# startplasma iterates them. NB: 06 deliberately does NOT export DISPLAY, so
# kwin's platform autodetect is not tricked into nested-X11.
_envdir="$HOME/.config/plasma-workspace/env"
if [ -d "$_envdir" ]; then
    for _f in "$_envdir"/*.sh; do
        [ -f "$_f" ] && . "$_f"
    done
    unset _f
fi

# Qt routes qWarning/qCritical to the journal socket when /run/systemd/journal/
# socket exists (it does -- schema-init's journal-sink owns it), NOT to stderr.
# Every Qt abort in this session was therefore SILENT in this log: the real
# message for the 2026-09-22 black screen ("kf.dbusaddons: DBus session bus not
# found") only appeared once stderr logging was forced by hand, after the dead
# broker had already been misdiagnosed as a stale plasmashell-shim. Force it so
# the next failure names itself here. (Crystal 2026-09-22)
export QT_FORCE_STDERR_LOGGING=1

# Compositor: direct kwin_wayland on the DRM backend. --drm forces the DRM
# platform (never the DISPLAY-autodetect nested-X11 trap); KWIN_DRM_DEVICES
# (=/dev/dri/card1, from the autologin env) selects the KMS node. Running
# outside any cgroup session-scope, sd_pid_get_session returns -ENODATA, so kwin
# skips schema-logind's incomplete Properties.Get(ss) seat path and opens the
# card directly. No trailing app arg — plasmashell is launched separately below
# (a kwin-argv-launched plasmashell crash-looped in testing; standalone is
# stable). kwin's stderr (kwin_*.debug rules from the autologin env) -> debug log.
/usr/bin/kwin_wayland --drm --xwayland >/home/ajax80/kwin-debug.log 2>&1 &
KWIN=$!
# schema-dbus-session-run.sh's broker-health watchdog kills this script's PID
# with a plain TERM when the session bus dies mid-session -- but a bare
# `kill` on a shell process doesn't propagate to backgrounded children, so
# without this trap kwin was left orphaned and running (still holding the
# card) while the bus and everything depending on it was gone -- the exact
# stuck state hand-recovered twice on 2026-09-22 (both SP4 cutover reboots).
# Killing kwin here makes `wait $KWIN` below return, so the script exits
# normally and the outer autologin loop's respawn actually takes over.
trap 'kill "$KWIN" 2>/dev/null' TERM INT HUP
i=0
while [ $i -lt 60 ]; do
    [ -S "${XDG_RUNTIME_DIR}/wayland-0" ] && break
    sleep 0.5
    i=$((i+1))
done
export WAYLAND_DISPLAY=wayland-0

# PipeWire stack. Normally systemd --user units; with schema-init as PID 1
# nothing provides the pipewire-0 socket -> no audio and kpipewire spins. Start
# in-session (seat access -> real ALSA sinks). Guarded: skip if already running.
if [ -x /usr/bin/pipewire ] && ! pgrep -x pipewire >/dev/null 2>&1; then
    /usr/bin/pipewire &
    sleep 1
    /usr/bin/wireplumber &
    sleep 1
    /usr/bin/pipewire-pulse &
fi
i=0
while [ $i -lt 20 ]; do
    [ -S "${XDG_RUNTIME_DIR}/pipewire-0" ] && break
    sleep 0.5
    i=$((i+1))
done
sleep 1

# Shell. plasmashell-shim (LD_PRELOAD=mock_sd.so, fakes sd_booted so
# plasmashell doesn't idle-spin on absent systemd --user) crash-aborts on
# every automatic boot post-SP4 (silent abort, no stderr) -- schema-dbus now
# emulates enough of org.freedesktop.systemd1.Manager that plain plasmashell
# runs fine without the fake; schema-plasma-watchdog.sh's fallback respawn
# (plain plasmashell, no shim) proved this live 2026-09-22, stable 15+ min.
# Launch bare directly instead of waiting on the watchdog's ~60-90s recovery.
pgrep -x plasmashell >/dev/null || /usr/bin/plasmashell &

# KDE session daemons ksmserver/startplasma normally start. Their
# /etc/xdg/autostart entries carry X-systemd-skip=true, so the KDE autostart
# fallback skips them too -> no ksycoca service daemon (kded6), no power
# management, no global shortcuts, no polkit auth prompts. Start in-session so
# they inherit the logind session (polkit-kde needs it to resolve its subject).
# Guarded against doubles (each is also a single-instance bus name).
#
# kactivitymanagerd and xdg-desktop-portal-kde are launched here too, not left
# to D-Bus activation: both have .service files under
# /usr/share/dbus-1/services, but an activated child only inherits
# schema-dbus's own process environ (see sdbus_activate_build_env in
# sdbus_activate.h), and that environ can be missing DBUS_SESSION_BUS_ADDRESS
# depending on how the broker itself was started (schema-dbus-session-run.sh
# always exports it first, but a hand-restarted broker may not) -- the
# activated child then can't reach dbus_bus_get(DBUS_BUS_SESSION) and exits
# before claiming its name. Direct launch here sidesteps that: it inherits
# this script's own env, which is always correct. Found 2026-09-22 (first
# reboot of the SP4 session-bus cutover): without kactivitymanagerd,
# plasmashell aborted shell load; without xdg-desktop-portal-kde,
# xdg-desktop-portal silently fell back to the gtk backend, breaking every
# KDE-only portal interface (InputCapture, ScreenCast, GlobalShortcuts,
# Clipboard, Wallpaper, RemoteDesktop, Usb) -- e.g. deskflow-core's
# InputCapture call failed with UnknownMethod.
for _svc in /usr/bin/kded6 \
            /usr/libexec/org_kde_powerdevil \
            /usr/libexec/kglobalacceld \
            /usr/libexec/kf6/polkit-kde-authentication-agent-1 \
            /usr/libexec/kactivitymanagerd \
            /usr/libexec/xdg-desktop-portal-kde; do
    [ -x "$_svc" ] && ! pgrep -f "$_svc" >/dev/null 2>&1 && "$_svc" &
done

# The session lives as long as the compositor. When kwin exits, the autologin
# loop re-evaluates its exit code (0 -> respawn, non-zero -> stop at console).
wait $KWIN
