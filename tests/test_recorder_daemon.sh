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
printf 'ts=17000 src=cor' >> "$(ls "$SCHEMA_RECORDER_DIR"/events-*.log | tail -1)"
start
OUT=$("$T.chk") || fail "shm after restart"
set -- $OUT
[ "$2" -gt 0 ] || fail "ring empty after restart"
$BIN send src=pid1 kind=died subj=afterTorn
sleep 0.3
grep -q '^ts=17000 src=cor$' "$SCHEMA_RECORDER_DIR"/events-*.log || fail "torn line not terminated"
grep -q 'subj=afterTorn' "$SCHEMA_RECORDER_DIR"/events-*.log || fail "post-restart line missing"
[ "$(stat -c %a "$(ls "$SCHEMA_RECORDER_DIR"/events-*.log | tail -1)")" = 600 ] || fail "events file mode"
ls "$SCHEMA_RECORDER_DIR" | grep -q '^events-[0-9]\{4\}-[0-9][0-9]-[0-9][0-9]\.log$' || fail "file name format"
stop

NOW=$(date +%s)
export SCHEMA_RECORDER_DIR=$T/ev2
mkdir -p "$SCHEMA_RECORDER_DIR"
printf 'ts=%s src=psi kind=episode-live comm=sh pid=1 rc=0 crit=0 subj=sh tier=4 msg=CPU pinned 3 min by sh\n' $((NOW - 14400)) > "$SCHEMA_RECORDER_DIR/events-x.log"
printf 'ts=%s src=psi kind=episode-end comm=sh pid=1 rc=0 crit=0 subj=sh tier=1 msg=CPU pinned 120 min by sh\n' $((NOW - 7200)) >> "$SCHEMA_RECORDER_DIR/events-x.log"
start
$BIN send src=coredump kind=crash comm=kwin_wayland pid=7 rc=11
$BIN send src=session kind=plasma-exit rc=1
sleep 0.5
OUT=$("$T.chk") || fail "shm reload"
echo "$OUT" | grep -q "kwin_wayland crashed" || fail "session arrow did not replace resolved live: $OUT"
stop
export SCHEMA_RECORDER_DIR=$T/ev

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
