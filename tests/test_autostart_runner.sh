#!/bin/sh
# schema-autostart-runner must hand autostart entries the compositor's
# WAYLAND_DISPLAY when plasmashell is slow (first login on a fresh install took
# >20 s; the wizard launched with no display and yad never drew, #486), and
# fall back to kwin's socket when plasmashell never comes.
# Runs in a user+pid namespace so the host's own plasmashell is invisible.
# SCHEMA_RUNNER_TICK=0.05 shrinks the runner's 20 s socket grace to 2 s.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
RUNNER="$HERE/distros/fedora-kde/scripts/schema-autostart-runner.sh"
# Docker's default seccomp profile refuses user namespaces.
unshare -r -p -f --mount-proc true 2>/dev/null \
    || { echo "SKIP: autostart runner (no unprivileged pid namespace)"; exit 0; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

mkdir -p "$T/home/.config/autostart" "$T/home/.local/bin" "$T/run" "$T/bin"
chmod 700 "$T/run"
printf '#!/bin/sh\nexit 0\n' > "$T/home/.local/bin/schema-plasma-watchdog.sh"
chmod +x "$T/home/.local/bin/schema-plasma-watchdog.sh"
# A script, not a copy of sleep: multi-call coreutils picks its applet from argv[0].
printf '#!/bin/sh\nwhile :; do sleep 1; done\n' > "$T/bin/plasmashell"
chmod +x "$T/bin/plasmashell"
printf '[Desktop Entry]\nType=Application\nExec=sh -c "env > %s/seen"\n' "$T" \
    > "$T/home/.config/autostart/probe.desktop"

# $1: shell snippet run in the background beside the runner
run() {
    rm -f "$T/seen"
    unshare -r -p -f --mount-proc env -i PATH="$PATH" HOME="$T/home" XDG_RUNTIME_DIR="$T/run" \
        SCHEMA_RUNNER_TICK=0.05 T="$T" RUNNER="$RUNNER" SETUP="$1" sh -c '
        (eval "$SETUP") &
        "$RUNNER"
        i=0; while [ ! -s "$T/seen" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
    '
}

fail=0
run 'sleep 3; WAYLAND_DISPLAY=wayland-ps exec "$T/bin/plasmashell"'
grep -qx 'WAYLAND_DISPLAY=wayland-ps' "$T/seen" 2>/dev/null \
    || { echo "FAIL: slow plasmashell: entry did not get its WAYLAND_DISPLAY"; fail=1; }

run 'python3 -c "import socket,time; s=socket.socket(socket.AF_UNIX); s.bind(\"$T/run/wayland-7\"); time.sleep(30)"'
grep -qx 'WAYLAND_DISPLAY=wayland-7' "$T/seen" 2>/dev/null \
    || { echo "FAIL: no plasmashell: entry did not get kwin's socket"; fail=1; }

LOG="$T/home/.local/state/schema-user/log/autostart-runner.log"
[ "$(grep -c '^=== runner start' "$LOG" 2>/dev/null)" = 2 ] \
    || { echo "FAIL: log does not keep both logins ($LOG)"; fail=1; }

[ $fail -eq 0 ] && echo "PASS: autostart runner waits for plasmashell, falls back to kwin socket"
exit $fail
