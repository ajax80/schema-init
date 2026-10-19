#!/bin/sh
set -u
cd "$(dirname "$0")/.."
BIN=./schema-recorder
[ -x "$BIN" ] || { echo "FAIL: no $BIN"; exit 1; }
T=$(mktemp -d)
export SCHEMA_RECORDER_SOCK=$T/run/r.sock
export SCHEMA_RECORDER_SHM=/schema-recorder-test-$$
export SCHEMA_RECORDER_DIR=$T/ev
export SCHEMA_RECORDER_NOPSI=1
PID=
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$T"; [ -x "$T.chk" ] && "$T.chk" unlink; rm -f "$T.chk" "$T.chk.c"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }

cat > "$T.chk.c" <<'CEOF'
#include "recorder_logic.h"
#include <fcntl.h>
int main(int c, char **v) {
    int fd = shm_open(getenv("SCHEMA_RECORDER_SHM"), O_RDONLY, 0);
    rec_shm_t s;
    if (c > 1) { shm_unlink(getenv("SCHEMA_RECORDER_SHM")); return 0; }
    const rec_shm_t *m = fd < 0 ? NULL : rec_shm_map(fd);
    (void)v;
    if (!m || rec_snapshot_ring(m, &s)) return 1;
    printf("%u %u %d %s\n", s.seq, s.count, s.arrow.state, s.arrow.line);
    return 0;
}
CEOF
gcc -O2 -std=c99 -D_GNU_SOURCE -I. -o "$T.chk" "$T.chk.c" -lrt || fail "chk build"

start() {
    $BIN > "$T/d.log" 2>&1 &
    PID=$!
    i=0
    while [ ! -S "$SCHEMA_RECORDER_SOCK" ] && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done
    [ -S "$SCHEMA_RECORDER_SOCK" ] || fail "daemon did not bind"
}
stop() { kill -TERM "$PID"; wait "$PID"; rc=$?; PID=; [ $rc -eq 0 ] || fail "daemon exit $rc"; }
nlines() { cat "$SCHEMA_RECORDER_DIR"/events-*.log 2>/dev/null | wc -l; }

start
$BIN send src=coredump kind=crash comm=kwin_wayland pid=7 rc=11 msg=SIGSEGV
$BIN send src=session kind=plasma-exit rc=0
sleep 0.5
F=$(cat "$SCHEMA_RECORDER_DIR"/events-*.log)
echo "$F" | grep -q "comm=kwin_wayland" || fail "crash line missing"
echo "$F" | grep -q "kind=plasma-exit" || fail "session line missing"
OUT=$("$T.chk") || fail "shm unreadable"
set -- $OUT
[ "$2" = 2 ] || fail "ring count $2 != 2"
[ "$3" = 2 ] || fail "arrow state $3 != 2"
echo "$OUT" | grep -q "kwin_wayland crashed (SIGSEGV)" || fail "arrow line: $OUT"
S1=$1

i=0
while [ $i -lt 1000 ]; do $BIN send src=pid1 kind=started subj=flood$i; i=$((i+1)); done
sleep 0.5
kill -0 "$PID" 2>/dev/null || fail "daemon died under flood"
[ "$(nlines)" -ge 3 ] || fail "flood not recorded"
OUT=$("$T.chk") || fail "shm after flood"
set -- $OUT
[ "$1" -gt "$S1" ] || fail "seq did not advance"

stop
printf 'ts=17000 src=cor\n' >> "$(ls "$SCHEMA_RECORDER_DIR"/events-*.log | tail -1)"
start
OUT=$("$T.chk") || fail "shm after restart"
set -- $OUT
[ "$2" -gt 0 ] || fail "ring empty after restart"
stop

s=$(date +%s%N)
$BIN send src=pid1 kind=died subj=x || fail "send nonzero"
e=$(date +%s%N)
[ $(( (e - s) / 1000000 )) -lt 1000 ] || fail "send slow"

start
sleep 1
a=$(awk '/^voluntary_ctxt_switches/{print $2}' /proc/$PID/status)
sleep 10
b=$(awk '/^voluntary_ctxt_switches/{print $2}' /proc/$PID/status)
[ $((b - a)) -le 1 ] || fail "idle wakeups $((b - a))"
stop
echo "test_recorder_daemon: OK"
