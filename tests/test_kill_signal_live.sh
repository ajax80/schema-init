#!/bin/sh
# audit 4.3: kill_signal= and the runtime stop escalation through the real
# engine (schema-user, no delegated cgroups here: main-process behaviour only;
# kill_mode's cgroup sweep is covered by tests/livetest)
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_kill_signal_live: skipped (root)"; exit 0; }
[ -x ./schema-user ] && [ -x ./schema-ctl ] || { echo "FAIL: build schema-user and schema-ctl first"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state XDG_CONFIG_HOME=$T/cfg SCHEMA_USER_IMPORT=
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID=
cleanup() { [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null; pkill -KILL -f "$T/" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; ./schema-ctl --user status 2>&1 | tail -8; tail -8 "$T/out"; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 80 ] && return 1; sleep 0.1; done; }
line() { ./schema-ctl --user status 2>&1 | grep -E "^ +$1 "; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"; mkdir -p "$S" "$T/cfg"; chmod 700 "$T/run/schema-user" "$S"
# records which signal ended it
printf 'name=si\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/si.pid; trap "echo INT > %s/si.got; exit 0" INT; trap "echo TERM > %s/si.got; exit 0" TERM; while :; do sleep 1 & wait; done\nkill_signal=INT\nstable_secs=1\n' "$T" "$T" "$T" > "$S/si.svc"
# ignores the stop signal: escalation after stop_timeout_sec
printf 'name=hard\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/hard.pid; trap "" TERM; while :; do sleep 1 & wait; done\nstop_timeout_sec=2\nstable_secs=1\n' "$T" > "$S/hard.svc"
# escalates with its own final_kill_signal
printf 'name=fin\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/fin.pid; trap "" TERM; trap "echo USR1 > %s/fin.got; exit 0" USR1; while :; do sleep 1 & wait; done\nstop_timeout_sec=2\nfinal_kill_signal=USR1\nstable_secs=1\n' "$T" "$T" > "$S/fin.svc"
chmod 600 "$S"/*.svc

./schema-user > "$T/out" 2>&1 &
PID=$!
until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"
for s in si hard fin; do until_ok "[ -s $T/$s.pid ]" || fail "$s never started"; done
sleep 1.2

r=$(./schema-ctl --user stop si 2>&1) || fail "stop si: $r"
case "$r" in "ok: SIGINT → si"*) ;; *) fail "stop reply names the signal: $r" ;; esac
until_ok "[ -s $T/si.got ]" || fail "si never got its kill_signal"
[ "$(cat "$T/si.got")" = INT ] || fail "si got $(cat "$T/si.got"), want INT"

./schema-ctl --user stop hard > /dev/null || fail "stop hard"
HP=$(cat "$T/hard.pid")
sleep 1
kill -0 "$HP" 2>/dev/null || fail "hard died before stop_timeout_sec"
until_ok "! kill -0 $HP 2>/dev/null" || fail "hard ignored TERM and was never escalated"
until_ok "line hard | grep -q state=EXCISED" || fail "hard: $(line hard)"
grep -q "hard.*stop-escalate" "$T/out" || fail "no stop-escalate log"

./schema-ctl --user stop fin > /dev/null || fail "stop fin"
until_ok "[ -s $T/fin.got ]" || fail "fin never got final_kill_signal=USR1"
echo "test_kill_signal_live: ok"
