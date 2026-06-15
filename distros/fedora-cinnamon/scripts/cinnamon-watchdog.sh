#!/bin/bash
# schema-init: keep the Cinnamon shell alive.
# No `systemd --user` to respawn `cinnamon` when it crashes, and cinnamon hosts
# the panel, menu/launcher and the systray -- so its death presents as apps
# "opening once, then refusing" (terminal launch still works, every click does
# nothing). cinnamon-session runs this via ~/.config/autostart at login.
# Exits cleanly when cinnamon-session is gone (logout); 60s backoff on a crash
# loop so a broken config can't make it hammer.
# (cinnamon-session is 16 chars -> pgrep -x can't match it, use -f.)
WD_LOG=/tmp/schema-cinnamon-watchdog.log

# Single instance: login-autostart and any manual/ssh start must never stack
# (two watchdogs would both respawn cinnamon -> duplicate shells on a crash).
LOCK="${XDG_RUNTIME_DIR:-/tmp}/schema-cinnamon-watchdog.lock"
exec 9>"$LOCK"
if ! flock -n 9; then
    echo "another watchdog already running, exit $(date)" >> "$WD_LOG"
    exit 0
fi

echo "watchdog start $(date)" > "$WD_LOG"

# A live cinnamon shell has a non-empty cmdline; a <defunct> zombie does not, so
# pgrep -x alone would treat a lingering zombie as "alive" and never respawn.
live_cinnamon_pid() {
    local p
    for p in $(pgrep -x cinnamon 2>/dev/null); do
        [ -n "$(tr -d '\0' < "/proc/$p/cmdline" 2>/dev/null)" ] && { echo "$p"; return 0; }
    done
    return 1
}
cinnamon_alive() { live_cinnamon_pid >/dev/null; }

for _ in $(seq 1 40); do cinnamon_alive && break; sleep 0.5; done

# Capture the live cinnamon's exact argv + full environment so a respawn is
# identical to how the session started it. This box runs `cinnamon --x11
# --replace`; a bare `cinnamon --replace` with a partial env starts then dies.
CPID=$(live_cinnamon_pid)
CINNAMON_ARGS=()
if [ -n "$CPID" ]; then
    while IFS= read -r -d '' a;  do CINNAMON_ARGS+=("$a"); done < "/proc/$CPID/cmdline"
    while IFS= read -r -d '' kv; do case "$kv" in *=*) export "$kv" 2>/dev/null ;; esac; done < "/proc/$CPID/environ" 2>/dev/null
fi
[ ${#CINNAMON_ARGS[@]} -eq 0 ] && CINNAMON_ARGS=(cinnamon --x11 --replace)
echo "respawn cmd: ${CINNAMON_ARGS[*]}  (DISPLAY=$DISPLAY)" >> "$WD_LOG"

fails=0; window=$(date +%s)
while sleep 5; do
    pgrep -f cinnamon-session >/dev/null 2>&1 || { echo "session gone, exit $(date)" >>"$WD_LOG"; break; }
    cinnamon_alive && continue
    now=$(date +%s)
    [ $((now - window)) -gt 60 ] && { fails=0; window=$now; }
    fails=$((fails + 1))
    if [ "$fails" -gt 5 ]; then
        echo "cinnamon crash-looping (>5/60s), backing off 60s $(date)" >>"$WD_LOG"
        sleep 60; fails=0; window=$(date +%s); continue
    fi
    echo "cinnamon down, restart #$fails $(date)" >>"$WD_LOG"
    setsid nohup "${CINNAMON_ARGS[@]}" >/dev/null 2>&1 &
    sleep 3
done
