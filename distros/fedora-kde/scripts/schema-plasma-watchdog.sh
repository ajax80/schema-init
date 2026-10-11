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

# Spin detector. A live plasmashell can still be dead to the user: its main
# thread runs model/DB work in signal handlers, so a churning source (ksycoca
# rebuild ping-pong 2026-06/08, duplicate kactivitymanagerd 2026-10-10) pins it
# at 100% and the taskbar stops taking clicks. Respawning doesn't help while the
# source churns, so capture evidence instead: main thread >=90% CPU for 60s ->
# stack trace to SPIN_DIR + desktop notification, once per pid.
SPIN_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/schema-plasma-spin"
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
spin_pid=""; spin_prev=0; spin_hot=0; spin_reported=""

spin_check() {
    local pid=$1 t
    t=$(awk '{print $14+$15}' "/proc/$pid/task/$pid/stat" 2>/dev/null) || return
    if [ "$pid" != "$spin_pid" ]; then
        spin_pid=$pid; spin_prev=$t; spin_hot=0; return
    fi
    if [ $((t - spin_prev)) -ge $((HZ * 5 * 9 / 10)) ]; then
        spin_hot=$((spin_hot + 1))
    else
        spin_hot=0
    fi
    spin_prev=$t
    [ "$spin_hot" -ge 12 ] && [ "$spin_reported" != "$pid" ] || return
    spin_reported=$pid
    mkdir -p "$SPIN_DIR"
    local f="$SPIN_DIR/$(date +%Y%m%d-%H%M%S)-$pid.txt"
    {
        echo "plasmashell $pid main thread >=90% CPU for 60s at $(date)"
        echo; echo "== main thread stack =="
        if command -v gdb >/dev/null 2>&1; then
            timeout 30 gdb -p "$pid" -batch -ex bt 2>/dev/null | grep '^#'
        elif command -v eu-stack >/dev/null 2>&1; then
            timeout 30 eu-stack -p "$pid" 2>/dev/null | sed -n "/^TID $pid:/,/^TID/p"
        else
            echo "(no gdb/eu-stack installed)"
        fi
        echo; echo "== duplicate session daemons =="
        for d in kactivitymanagerd kded6 xdg-desktop-portal-kde; do
            echo "$d: $(pgrep -fc "/$d\$" 2>/dev/null)"
        done
        echo; echo "== ksycoca mtimes =="
        ls -l --time-style=+%T "$HOME"/.cache/ksycoca6_* 2>/dev/null
    } >"$f" 2>&1
    echo "plasmashell $pid spinning, evidence in $f $(date)" >>"$WD_LOG"
    command -v notify-send >/dev/null 2>&1 && notify-send -u critical -a "schema-init" \
        "Plasma panel is spinning" "plasmashell main thread pinned at 100% for 60s. Evidence saved to $f" &
}

while sleep 5; do
    pgrep -x kwin_wayland >/dev/null 2>&1 || { echo "compositor gone, exit $(date)" >>"$WD_LOG"; break; }
    psh=$(pgrep -x plasmashell | head -1)
    if [ -n "$psh" ]; then
        spin_check "$psh"
        continue
    fi
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
