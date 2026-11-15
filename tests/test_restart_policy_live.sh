#!/bin/sh
# audit 4.2: restart= / success_exit_status= / restart_prevent_exit_status=
# through the real engine (schema-user runs PID 1's engine.c, unprivileged)
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_restart_policy_live: skipped (root)"; exit 0; }
[ -x ./schema-user ] && [ -x ./schema-ctl ] || { echo "FAIL: build schema-user and schema-ctl first"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state XDG_CONFIG_HOME=$T/cfg SCHEMA_USER_IMPORT=
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID=
cleanup() { [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null; pkill -KILL -f "$T/" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; ./schema-ctl --user status 2>&1 | tail -12; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 80 ] && return 1; sleep 0.1; done; }
st() { ./schema-ctl --user status 2>&1; }
line() { st | grep -E "^ +$1 "; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"; mkdir -p "$S" "$T/cfg"; chmod 700 "$T/run/schema-user" "$S"
# each run appends a line, then ends the way its name says
svc() { # name, how the run ends, extra keys
    printf 'name=%s\nexec=/bin/sh\nargs=-c\nargs=echo r >> %s/%s.runs; sleep 0.3; %s\nrestart_sec=0\n%b' "$1" "$T" "$1" "$2" "$3" > "$S/$1.svc"
}
svc always0  'exit 0'        ''
svc onfail0  'exit 0'        'restart=on-failure\n'
svc onfail1  'exit 1'        'restart=on-failure\n'
svc onfailtm 'kill -TERM $$' 'restart=on-failure\n'
svc no1      'exit 1'        'restart=no\n'
svc abort1   'exit 1'        'restart=on-abort\n'
svc abortsg  'kill -SEGV $$' 'restart=on-abort\n'
svc succ7    'exit 7'        'restart=on-failure\nsuccess_exit_status=7\n'
svc prev5    'exit 5'        'restart_prevent_exit_status=5\n'
svc onsucc1  'exit 1'        'restart=on-success\n'
svc os7      'exit 7'        'oneshot=1\nsuccess_exit_status=7\n'
chmod 600 "$S"/*.svc

./schema-user > "$T/out" 2>&1 &
PID=$!
until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"
runs() { [ -f "$T/$1.runs" ] && wc -l < "$T/$1.runs" || echo 0; }

# restarted: a second run shows up
for s in always0 onfail1 abortsg; do
    until_ok "[ \$(runs $s) -ge 2 ]" || fail "$s should restart (runs=$(runs $s))"
done
# stays down: EXCISED after one run
for s in onfail0 onfailtm no1 abort1 succ7 prev5 onsucc1; do
    until_ok "line $s | grep -q 'state=EXCISED'" || fail "$s should stay down: $(line $s)"
done
sleep 1.5
for s in onfail0 onfailtm no1 abort1 succ7 prev5 onsucc1; do
    [ "$(runs $s)" = 1 ] || fail "$s ran $(runs $s) times"
done
until_ok "line os7 | grep -q 'state=PERFECT'" || fail "oneshot exit 7 with success_exit_status=7: $(line os7)"
grep -q "onfail0.*76-restart-policy\|76-restart-policy.*onfail0" "$T/out" || grep -rq "76-restart-policy" "$T/state" 2>/dev/null \
    || fail "no 76-restart-policy log line"

# exit 0 or a success_exit_status= code reads as stopped (reason none); a failure keeps its reason
./schema-ctl --user status --kv 2>/dev/null > "$T/kv"
grep -q "^service.onfail0.excise_reason=none" "$T/kv" || fail "onfail0 reason: $(grep '^service.onfail0.excise' "$T/kv")"
grep -q "^service.no1.excise_reason=exit-code" "$T/kv" || fail "no1 reason: $(grep '^service.no1.excise' "$T/kv")"
grep -q "^service.succ7.excise_reason=none" "$T/kv" || fail "succ7 reason: $(grep '^service.succ7.excise' "$T/kv")"

# start brings a policy-stopped service back, and the policy still holds after
./schema-ctl --user start onfail0 > /dev/null || fail "start onfail0"
until_ok "[ \$(runs onfail0) -ge 2 ] && line onfail0 | grep -q 'state=EXCISED'" || fail "onfail0 after start: $(line onfail0)"
echo "test_restart_policy_live: ok"
