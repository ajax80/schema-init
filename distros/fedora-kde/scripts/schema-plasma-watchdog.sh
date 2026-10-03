#!/bin/bash
# schema-init Track B: plasmashell watchdog.
# No `systemd --user` to respawn plasmashell, so a crash leaves the launcher and
# system tray dead -- apps then "open once, then refuse" because click-to-launch
# and the tray host live inside plasmashell. Keep it alive while the Wayland
# session lives; exit cleanly once kwin_wayland is gone (logout).
#
# MUST be launched detached (setsid nohup) from schema-autostart-runner.sh:
# the runner is a setsid session leader, so a bare `( ) &` child takes SIGHUP
# when the runner exits and dies before it can ever respawn anything.
WD_LOG=/tmp/schema-plasma-watchdog.log

# Single instance: never let two watchdogs run (both would respawn plasmashell).
LOCK="${XDG_RUNTIME_DIR:-/tmp}/schema-plasma-watchdog.lock"
exec 9>"$LOCK"
flock -n 9 || { echo "another watchdog already running, exit $(date)" >> "$WD_LOG"; exit 0; }

echo "watchdog start $(date)" > "$WD_LOG"
fails=0; window=$(date +%s)

# One-time self-heal at login. The initial plasma-session-forked plasmashell can
# come up WITHOUT XDG_MENU_PREFIX=plasma- (a race in startplasma's env propagation
# across the duplicated plasma-session tree schema-init autostart creates -- clean
# on a relogin, bare on some cold boots). A bare-but-alive plasmashell builds
# ksycoca from applications.menu while the dbus-activated cohort (kded6, ksmserver)
# builds plasma-applications.menu -> dueling rebuilds of the same cache -> IO storm
# -> mouse/keyboard/YouTube stutter, jumping Kickoff, ?-icons. The absence-only loop
# below never catches this because plasmashell IS running. Wait for it, and if it is
# bare, kill it so the respawn below relaunches it with the correct env. Fires only
# when the race lands dirty; a clean boot leaves it untouched. (Claire 2026-08-18)
for _ in $(seq 1 30); do pgrep -x plasmashell >/dev/null 2>&1 && break; sleep 1; done
psh=$(pgrep -x plasmashell | head -1)
if [ -n "$psh" ] && ! grep -qz 'XDG_MENU_PREFIX=plasma-' /proc/"$psh"/environ 2>/dev/null; then
    echo "initial plasmashell $psh has bare XDG_MENU_PREFIX, killing for clean respawn $(date)" >>"$WD_LOG"
    # SIGKILL, not SIGTERM: plasmashell catches SIGTERM as a session-save and does
    # NOT exit, so the self-heal silently no-oped and left the bare shell thrashing
    # ksycoca every boot (proven live 2026-08-18: same pid survived `kill`). -9 forces
    # exit; the while-loop below then respawns it WITH XDG_MENU_PREFIX=plasma-. (Claire)
    kill -9 "$psh"
    # SIGKILL can't release the ksycoca build lock the bare shell held -> the
    # respawned shell inherits a stale *.lock owned by a dead pid and its
    # menu/applet resolution wedges -> panels never draw (proven live 2026-08-18:
    # lock still named the -9'd pid, taskbar blank). Clear stale locks so the
    # respawn builds sycoca clean. (Claire)
    rm -f "${HOME}/.cache/ksycoca6"*.lock
fi

while sleep 5; do
    pgrep -x kwin_wayland >/dev/null 2>&1 || { echo "compositor gone, exit $(date)" >>"$WD_LOG"; break; }
    pgrep -x plasmashell  >/dev/null 2>&1 && continue
    now=$(date +%s)
    [ $((now - window)) -gt 60 ] && { fails=0; window=$now; }
    fails=$((fails + 1))
    if [ "$fails" -gt 5 ]; then
        echo "plasmashell crash-looping (>5/60s), backing off 60s $(date)" >>"$WD_LOG"
        sleep 60; fails=0; window=$(date +%s); continue
    fi
    echo "plasmashell down, restart #$fails $(date)" >>"$WD_LOG"
    # A crash/-9 mid-sycoca-build orphans ksycoca6*.lock (dead pid) -> the respawn
    # wedges on menu/applet resolution -> black desktop. Clear before respawn. (Claire)
    rm -f "${HOME}/.cache/ksycoca6"*.lock
    # Respawn with the Plasma menu env so plasmashell builds the same ksycoca as
    # kded6 (plasma-applications.menu). Without this the shell forks bare -> two
    # cohorts fight over one cache file -> IO-storm stutter. (Claire 2026-08-17)
    export XDG_MENU_PREFIX=plasma-
    export XDG_CONFIG_DIRS="$HOME/.config/kdedefaults:/etc/xdg:/usr/share/kde-settings/kde-profile/default/xdg"
    # WAYLAND_DISPLAY: the watchdog starts before kwin creates the socket, so its own
    # env lacks it -> a respawned plasmashell runs HEADLESS (alive but no taskbar/icons,
    # never connects to the compositor). Resolve it live from the runtime socket at
    # respawn time. Without this the self-heal kill-9 above trades a stuttering-visible
    # shell for an invisible one. (Claire 2026-08-18)
    if [ -z "$WAYLAND_DISPLAY" ]; then
        sock=$(ls "${XDG_RUNTIME_DIR:-/run/user/1000}"/wayland-[0-9] 2>/dev/null | head -1)
        [ -n "$sock" ] && export WAYLAND_DISPLAY="${sock##*/}"
    fi
    # Force a full sycoca rebuild before respawn. Clearing the lock + a fresh
    # *incremental* cache is STILL not enough: proven live 2026-08-18, a respawn
    # came up alive+connected+correct-env+no-lock yet never spawned the desktop
    # kioworker (desktop.so) -> black desktop; it wedged on containment resolution
    # against the incremental cache. Only `kbuildsycoca6 --noincremental` before
    # the launch cleared the wedge (new shell spawned the kioworker, rendered).
    # This is a NEW failure mode past the lock/env/headless ones. (Claire)
    kbuildsycoca6 --noincremental >/dev/null 2>&1
    setsid nohup plasmashell >/dev/null 2>&1 &
    # No post-launch "render verify": it waited for a desktop.so kioworker, but
    # current KIO runs that worker in-process, so the check never passed and
    # kill -9'd every healthy respawn (3x, until the crash-loop cap). On a first
    # login that interrupted Plasma writing its default layout -> a panel with
    # no task manager, tray or clock (fresh ISO install, 2026-10-03).
done
