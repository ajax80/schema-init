#!/bin/sh
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_schema_user: skipped (root)"; exit 0; }
[ -x ./schema-user ] || { echo "FAIL: no ./schema-user"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state/nested XDG_CONFIG_HOME=$T/cfg
# hand-written .svc files below: no import over them
export SCHEMA_USER_IMPORT=
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
{ sleeper held; echo "wait_socket=$T/run/never"; } > "$S/held.svc"
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
SHM=/dev/shm/schema-user-$(id -u)
[ "$(stat -c %a "$SHM")" = 600 ] || fail "shm $SHM mode $(stat -c %a "$SHM")"

# a user may edit their own .svc and reload it; PID 1's since-boot rule is not theirs
echo "stable_secs=2" >> "$S/a.svc"
./schema-ctl --user reload > "$T/reload" 2>&1 || fail "reload of an edited .svc: $(cat "$T/reload")"

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
st | grep "waiting=" | grep -vq "^ *held " && { st; fail "waiting= still shown"; }
# a held service that is stopped is no longer waiting, and no longer wakes us
./schema-ctl --user stop held > /dev/null || fail "stop held"
st | grep -q "waiting=" && { st; fail "stopped service still shown waiting"; }

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
# --- schema-user runs schema-import --user at start (SCHEMA_USER_IMPORT) ---
# start a fresh instance on its own tree; $1 = importer path ("" = none)
istart() {
    I=$(mktemp -d "$T/imp.XXXX")
    mkdir -m700 "$I/run"
    SCHEMA_USER_IMPORT=$1 XDG_RUNTIME_DIR=$I/run XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg \
        ./schema-user > "$I/out" 2>&1 &
    PID=$!
}
ist() { XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status 2>/dev/null; }
istop() { kill -TERM "$PID"; wait "$PID"; PID=; }
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$T/$1"; chmod 755 "$T/$1"; echo "$T/$1"; }

# import_runs: the stub's .svc lands in --out and runs
S1=$(stub imp-ok 'while [ $# -gt 0 ]; do [ "$1" = --out ] && o=$2; shift; done
printf "name=x\nexec=%s\nargs=30\n" "'"$T/sl"'" > "$o/x.svc"; chmod 600 "$o/x.svc"')
istart "$S1"
until_ok 'ist | grep -Eq "^ +x +pid=[1-9]"' || { ist; cat "$I/out"; fail "import_runs: x not running"; }
grep -q "import: exit 0" "$I/out" || fail "import_runs: no exit line"
istop

# import_fail: exit 1 -> still serves status
S2=$(stub imp-fail 'exit 1')
istart "$S2"
until_ok 'ist | grep -q "^services:"' || fail "import_fail: no status"
grep -q "import: exit 1" "$I/out" || fail "import_fail: no exit 1 line"
istop

# import_hang: killed after 10 s, then serves status
S3=$(stub imp-hang "exec $T/sl 60")
istart "$S3"
i=0; until ist | grep -q "^services:"; do i=$((i + 1)); [ $i -ge 130 ] && fail "import_hang: no status in 13 s"; sleep 0.1; done
grep -q "import: killed after 10 s" "$I/out" || fail "import_hang: no kill line"
pgrep -f "$T/sl 60" > /dev/null && fail "import_hang: importer left running"
istop

# import_disabled: empty SCHEMA_USER_IMPORT keeps a hand-written .svc
I0=$(mktemp -d "$T/imp.XXXX"); mkdir -m700 "$I0/run" "$I0/run/schema-user"; mkdir -m700 "$I0/run/schema-user/services"
printf 'name=h\nexec=%s\nargs=30\n' "$T/sl" > "$I0/run/schema-user/services/h.svc"; chmod 600 "$I0/run/schema-user/services/h.svc"
I=$I0
SCHEMA_USER_IMPORT= XDG_RUNTIME_DIR=$I/run XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg ./schema-user > "$I/out" 2>&1 &
PID=$!
until_ok 'ist | grep -Eq "^ +h +pid=[1-9]"' || { ist; fail "import_disabled: h not running"; }
grep -q "import: not run" "$I/out" || fail "import_disabled: no 'not run' line"
# status lists the skip manifest (text and --kv), never in --json
printf 'syncthing.service\thandled elsewhere (skip list)\n' > "$I/run/schema-user/skipped"
ist | grep -Eq '^ +syncthing\.service +skipped \(handled elsewhere \(skip list\)\)$' || { ist; fail "status: no skipped line"; }
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --kv | grep -qx 'service.syncthing.service.state=skipped' || fail "status --kv: no skipped state"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --kv | grep -qx 'service.syncthing.service.reason=handled elsewhere (skip list)' || fail "status --kv: no reason"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --json | grep -q syncthing && fail "status --json: lists skipped"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status h | grep -q syncthing && fail "status <svc>: lists skipped"
istop

echo "test_schema_user: ok"
