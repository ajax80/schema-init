#!/bin/sh
# schema-autostart-runner: an autostart entry must get the compositor's
# WAYLAND_DISPLAY even when plasmashell comes up slowly. A first login on a
# fresh install took >20 s; the runner gave up waiting, launched the first-boot
# wizard with no display, and yad never drew (#486).
# Runs in a user+pid namespace so the host's own plasmashell is invisible.
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
cp "$(command -v sleep)" "$T/bin/plasmashell"
printf '[Desktop Entry]\nType=Application\nExec=sh -c "env > %s/seen"\n' "$T" \
    > "$T/home/.config/autostart/probe.desktop"

unshare -r -p -f --mount-proc env -i PATH="$PATH" HOME="$T/home" XDG_RUNTIME_DIR="$T/run" \
    T="$T" RUNNER="$RUNNER" sh -c '
    (sleep 25; WAYLAND_DISPLAY=wayland-test exec "$T/bin/plasmashell" 120) &
    "$RUNNER"
    i=0; while [ ! -s "$T/seen" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
'

LOG="$T/home/.local/state/schema-user/log/autostart-runner.log"
fail=0
grep -qx 'WAYLAND_DISPLAY=wayland-test' "$T/seen" 2>/dev/null \
    || { echo "FAIL: entry launched without WAYLAND_DISPLAY"; fail=1; }
[ -s "$LOG" ] || { echo "FAIL: no log at $LOG"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: autostart runner waits for slow plasmashell"
exit $fail
