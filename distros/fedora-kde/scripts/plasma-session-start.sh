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

# Shell. plasmashell-shim preloads mock_sd.so (fakes sd_booted so plasmashell
# doesn't idle-spin waiting on the absent systemd --user).
pgrep -x plasmashell >/dev/null || /usr/local/bin/plasmashell-shim &

# KDE session daemons ksmserver/startplasma normally start. Their
# /etc/xdg/autostart entries carry X-systemd-skip=true, so the KDE autostart
# fallback skips them too -> no ksycoca service daemon (kded6), no power
# management, no global shortcuts, no polkit auth prompts. Start in-session so
# they inherit the logind session (polkit-kde needs it to resolve its subject).
# Guarded against doubles (each is also a single-instance bus name).
for _svc in /usr/bin/kded6 \
            /usr/libexec/org_kde_powerdevil \
            /usr/libexec/kglobalacceld \
            /usr/libexec/kf6/polkit-kde-authentication-agent-1; do
    [ -x "$_svc" ] && ! pgrep -f "$_svc" >/dev/null 2>&1 && "$_svc" &
done

# The session lives as long as the compositor. When kwin exits, the autologin
# loop re-evaluates its exit code (0 -> respawn, non-zero -> stop at console).
wait $KWIN
