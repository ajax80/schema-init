#!/bin/sh
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_schema_user: skipped (root)"; exit 0; }
[ -x ./schema-user ] || { echo "FAIL: no ./schema-user"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state/nested XDG_CONFIG_HOME=$T/cfg
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID= LPID=
cleanup() {
    [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null
    [ -n "$LPID" ] && kill -KILL "$LPID" 2>/dev/null
    pkill -KILL -f "$T/sl" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; [ -f "$T/out" ] && tail -20 "$T/out"; exit 1; }
# wait up to 5 s for a shell condition
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 50 ] && return 1; sleep 0.1; done; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"
mkdir -p "$S" "$T/cfg/environment.d"
chmod 700 "$T/run/schema-user" "$S"
printf 'SCHEMA_T=hi-${HOME}\n# comment\n\n' > "$T/cfg/environment.d/50-t.conf"

# sleepers stamp their SIGTERM time so stop order is observable
sleeper() {
    printf 'name=%s\nexec=/bin/sh\nargs=-c\nargs=trap "date +%%s%%N > %s/%s.stop; exit 0" TERM; while :; do sleep 1 & wait; done\nstable_secs=1\n' \
        "$1" "$T" "$1"
}
sleeper a > "$S/a.svc"
{ sleeper b; echo dep=a; } > "$S/b.svc"
{ sleeper busy; echo "wait_socket=$T/run/bus"; } > "$S/busy.svc"
printf 'name=flap\nexec=/bin/sh\nargs=-c\nargs=sleep 0.2; exit 1\n' > "$S/flap.svc"
printf 'name=dbl\nexec=/bin/sh\nargs=-c\nargs=sh -c '"'"'sleep 0.5; cut -d" " -f4 /proc/$$/stat > %s/dbl.ppid; exec %s/sl 30'"'"' & exit 0\noneshot=1\n' \
    "$T" "$T" > "$S/dbl.svc"
printf 'name=envdump\nexec=/bin/sh\nargs=-c\nargs=env > %s/env\noneshot=1\n' "$T" > "$S/envdump.svc"
printf 'name=tmr\nexec=/bin/echo\nargs=tick\non_calendar=03:00\npersistent=1\n' > "$S/tmr.svc"
chmod 600 "$S"/*.svc
# the orphan runs as $T/sl so its cmdline is unique to this run
ln -s "$(command -v sleep)" "$T/sl"

./schema-user > "$T/out" 2>&1 &
PID=$!

until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"
[ "$(stat -c %a "$T/run/schema-user/ctl.sock")" = 600 ] || fail "ctl.sock mode $(stat -c %a "$T/run/schema-user/ctl.sock")"
st() { ./schema-ctl --user status 2>&1; }
until_ok 'st | grep -Eq "^ +a +pid=[1-9][0-9]* +state=FUNDAMENTAL" && st | grep -Eq "^ +b +pid=[1-9][0-9]* +state=FUNDAMENTAL"' \
    || { st; fail "a/b not FUNDAMENTAL"; }
until_ok 'st | grep -Eq "^ +flap .* restarts=[1-9]"' || { st; fail "flap never restarted"; }

until_ok '[ -s "$T/env" ]' || fail "envdump did not run"
grep -q '^HOME=' "$T/env" || fail "no HOME"
grep -qx "XDG_RUNTIME_DIR=$T/run" "$T/env" || fail "XDG_RUNTIME_DIR"
grep -qx "DBUS_SESSION_BUS_ADDRESS=unix:path=$T/run/bus" "$T/env" || fail "DBUS_SESSION_BUS_ADDRESS"
grep -qx "SCHEMA_T=hi-$HOME" "$T/env" || fail "environment.d: $(grep SCHEMA_T "$T/env")"
grep -q '^WAYLAND_DISPLAY=' "$T/env" && fail "WAYLAND_DISPLAY leaked"

until_ok '[ -s "$T/dbl.ppid" ]' || fail "dbl grandchild never wrote ppid"
[ "$(cat "$T/dbl.ppid")" = "$PID" ] || fail "orphan reparented to $(cat "$T/dbl.ppid"), not schema-user $PID"

[ -f "$T/state/nested/schema-user/timers/tmr.stamp" ] || fail "no timer stamp"
[ -f "$T/state/nested/schema-user/log/a.log" ] || fail "no service log"

./schema-user > "$T/second" 2>&1 && fail "second instance started"
grep -q "already running" "$T/second" || fail "second instance: $(cat "$T/second")"
env -u XDG_RUNTIME_DIR ./schema-user 2>/dev/null && fail "ran without XDG_RUNTIME_DIR"
XDG_RUNTIME_DIR=/nonexistent ./schema-user 2>/dev/null && fail "ran with a missing XDG_RUNTIME_DIR"

./schema-ctl --user stop flap > /dev/null || fail "stop flap"
ctxsw() { awk '/^voluntary_ctxt_switches/ {print $2}' "/proc/$PID/status"; }

# wait_socket=: held while the socket is missing, and while it is stale
busy_waiting() { st | grep -Eq "^ +busy +pid=0 .*  waiting=$T/run/bus\$"; }
sleep 2
busy_waiting || { st; fail "busy not held on a missing socket"; }
python3 -c 'import socket, sys; s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.close()' "$T/run/bus"
[ -S "$T/run/bus" ] || fail "no stale socket made"
c0=$(ctxsw)
sleep 2
c1=$(ctxsw)
# a held service is re-checked once a second, not on the 250 ms tick (8)
[ $((c1 - c0)) -le 4 ] || fail "held service ticks: $((c1 - c0)) wakeups in 2 s"
busy_waiting || { st; fail "busy not held on a stale socket"; }
python3 -c 'import os, socket, sys, time
os.unlink(sys.argv[1]); s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(8); time.sleep(30)' "$T/run/bus" &
LPID=$!
until_ok 'st | grep -Eq "^ +busy +pid=[1-9][0-9]* +state=(FULL_TRUST|FUNDAMENTAL)"' \
    || { st; fail "busy not started once the socket accepts"; }
until_ok 'st | grep -Eq "^ +busy +pid=[1-9][0-9]* +state=FUNDAMENTAL"' || { st; fail "busy not FUNDAMENTAL"; }
st | grep -q "waiting=" && { st; fail "waiting= still shown"; }

sleep 0.5
c0=$(ctxsw)
sleep 3
c1=$(ctxsw)
# idle measures 0; a 250 ms tick is 12
[ $((c1 - c0)) -le 3 ] || fail "not idle: $((c1 - c0)) wakeups in 3 s"

kill -TERM "$PID"
until_ok '! kill -0 "$PID" 2>/dev/null' || fail "still running 5 s after SIGTERM"
wait "$PID"; rc=$?
PID=
[ $rc = 0 ] || fail "exit status $rc"
[ -s "$T/a.stop" ] && [ -s "$T/b.stop" ] || fail "sleepers saw no SIGTERM"
[ "$(cat "$T/b.stop")" -le "$(cat "$T/a.stop")" ] || fail "a stopped before its dependent b"
pgrep -f "$T/sl" > /dev/null && fail "dbl orphan survived"
echo "test_schema_user: ok"
