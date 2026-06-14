#!/bin/bash
# schema-init Track B: classic XDG autostart sweep.
# Plasma 6 defers ~/.config/autostart/*.desktop to xdg-desktop-autostart.target
# whenever sd_booted()==1 (now native via schema-init PR#10) -- but there is no
# `systemd --user` to run that target, so the entries are orphaned. We run the
# sweep ourselves. Launched from ~/.config/plasma-workspace/env/zz-schema-autostart.sh.

LOG=/tmp/schema-autostart-runner.log
echo "runner start $(date)" > "$LOG"

for _ in $(seq 1 40); do pgrep -x plasmashell >/dev/null && break; sleep 0.5; done
sleep 2

# ssh-agent on a fixed socket (matches env/ssh-agent-sock.sh) so git and
# Claude Code inherit a live agent. ssh-add -l: 0=keys, 1=running/empty, 2=no agent.
SSH_SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ssh-agent.socket"
export SSH_AUTH_SOCK="$SSH_SOCK"
ssh-add -l >/dev/null 2>&1
if [ $? -eq 2 ]; then
    rm -f "$SSH_SOCK"
    ssh-agent -a "$SSH_SOCK" >/dev/null 2>&1 && echo "ssh-agent started at $SSH_SOCK" >>"$LOG"
else
    echo "ssh-agent already up at $SSH_SOCK" >>"$LOG"
fi
# load key unattended via KWallet (ksshaskpass prompts once, stores in wallet, silent after)
if ! ssh-add -l >/dev/null 2>&1; then
    SSH_ASKPASS=/usr/bin/ksshaskpass SSH_ASKPASS_REQUIRE=force ssh-add ~/.ssh/id_ed25519 </dev/null >>"$LOG" 2>&1 \
        && echo "ssh key loaded" >>"$LOG" || echo "ssh-add failed (wallet locked?)" >>"$LOG"
fi

shopt -s nullglob
for f in "$HOME"/.config/autostart/*.desktop; do
    name=${f##*/}
    val() { grep -m1 "^$1=" "$f" | cut -d= -f2-; }

    t=$(val Type); [ -n "$t" ] && [ "$t" != "Application" ] && { echo "skip type=$t $name" >>"$LOG"; continue; }
    [ "$(val Hidden)" = "true" ] && { echo "skip hidden $name" >>"$LOG"; continue; }
    [ "$(val X-GNOME-Autostart-enabled)" = "false" ] && { echo "skip gnome-off $name" >>"$LOG"; continue; }
    [ "$(val X-KDE-autostart-phase)" = "0" ] && { echo "skip phase0(core) $name" >>"$LOG"; continue; }

    only=$(val OnlyShowIn); notin=$(val NotShowIn)
    [ -n "$only" ]  && [[ ";$only;"  != *";KDE;"* ]] && { echo "skip onlyshowin $name" >>"$LOG"; continue; }
    [ -n "$notin" ] && [[ ";$notin;" == *";KDE;"* ]] && { echo "skip notshowin $name" >>"$LOG"; continue; }

    tryexec=$(val TryExec)
    [ -n "$tryexec" ] && ! command -v "$tryexec" >/dev/null 2>&1 && [ ! -x "$tryexec" ] && { echo "skip tryexec $name" >>"$LOG"; continue; }

    cmd=$(val Exec | sed 's/ *%[A-Za-z]//g')
    [ -z "$cmd" ] && continue

    set -- $cmd
    prog=$1
    case "${prog##*/}" in
        sh|bash|dash|zsh|env|python|python2|python3|perl|ruby|node) shift; [ -n "$1" ] && prog=$1 ;;
    esac
    key=${prog##*/}
    if pgrep -f -- "$prog" >/dev/null 2>&1 || pgrep -x "$key" >/dev/null 2>&1; then
        echo "skip running $name ($key)" >>"$LOG"; continue
    fi

    echo "launch $name -> $cmd" >>"$LOG"
    setsid nohup sh -c "$cmd" >/dev/null 2>&1 &
done
# --- plasmashell watchdog -------------------------------------------------
# No `systemd --user` to respawn plasmashell, so a crash leaves the launcher
# and system tray dead -- apps then "open once, then refuse" because click-to-
# launch and the tray host live inside plasmashell. Keep it alive while the
# Wayland session lives; exit cleanly once kwin_wayland is gone (logout).
(
    WD_LOG=/tmp/schema-plasma-watchdog.log
    # Single instance: never let two watchdogs run (both would respawn plasmashell).
    LOCK="${XDG_RUNTIME_DIR:-/tmp}/schema-plasma-watchdog.lock"
    exec 9>"$LOCK"
    flock -n 9 || { echo "another watchdog already running, exit $(date)" >> "$WD_LOG"; exit 0; }
    echo "watchdog start $(date)" > "$WD_LOG"
    fails=0; window=$(date +%s)
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
        setsid nohup plasmashell >/dev/null 2>&1 &
        sleep 3
    done
) &

echo "runner done $(date)" >> "$LOG"
