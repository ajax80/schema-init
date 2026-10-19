#!/bin/sh
set -u
cd "$(dirname "$0")/../.."
HOLD=${1:-150}
EXIT=${2:-1}
T=/run/schema-recorder-replay
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 1; }
install -d -m 0700 "$T" "$T/bin" "$T/run"
[ "$(stat -c %u "$T")" = 0 ] || exit 1
export SCHEMA_RECORDER_SOCK=$T/run/r.sock
export SCHEMA_RECORDER_SHM=/schema-recorder-replay
export SCHEMA_RECORDER_DIR=$T/ev
for n in dolphin kioworker cosmic-player; do ln -sf /usr/bin/bash "$T/bin/$n"; done
RPID=
if ! [ -S "$SCHEMA_RECORDER_SOCK" ]; then
    ./schema-recorder > "$T/d.log" 2>&1 &
    RPID=$!
    echo $RPID > "$T/recorder.pid"
    sleep 1
fi
cleanup() { pkill -f "^$T/bin/" 2>/dev/null; }
trap cleanup EXIT INT TERM
taskset -c 10,11 "$T/bin/dolphin" -c "\"$T/bin/kioworker\" -c 'for i in 1 2 3 4 5 6 7 8; do \"$T/bin/cosmic-player\" -c \"while :; do :; done\" & done; wait'; :" &
echo "hogs on cpus 10-11 for ${HOLD}s"
sleep "$HOLD"
cleanup
sleep 2
[ "$EXIT" = 1 ] && ./schema-recorder send src=session kind=plasma-exit rc=0
sleep 2
./schema-board --once
echo ---
./schema-ctl trace
