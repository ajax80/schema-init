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

# No Xwayland cookie step: Plasma 6 kwin starts Xwayland on demand with no
# -auth and grants SI:localuser, so there is nothing to merge -- waiting for it
# only delayed every autostart app by 20s.

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
if [ ! -f ~/.ssh/id_ed25519 ]; then
    echo "no ssh key (~/.ssh/id_ed25519), skipping ssh-add" >>"$LOG"
elif ! ssh-add -l >/dev/null 2>&1; then
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
# Extracted to its own script and launched DETACHED. The runner is a setsid
# session leader; a bare `( ) &` child took SIGHUP on runner exit and died
# before it could respawn anything (caught via boot-capture, 2026-06-14).
# setsid+nohup puts the watchdog in its own session so it outlives the runner.
WD="$HOME/.local/bin/schema-plasma-watchdog.sh"
[ -x "$WD" ] || WD=/usr/local/lib/schema/schema-plasma-watchdog.sh
if [ -x "$WD" ]; then
    setsid nohup "$WD" >/dev/null 2>&1 &
    echo "watchdog launched (detached) $(date)" >> "$LOG"
else
    echo "watchdog script missing: $WD" >> "$LOG"
fi

echo "runner done $(date)" >> "$LOG"
