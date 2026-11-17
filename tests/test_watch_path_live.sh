#!/bin/sh
# audit 4.8: watch_path= services through the real engine (schema-user)
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_watch_path_live: skipped (root)"; exit 0; }
[ -x ./schema-user ] && [ -x ./schema-ctl ] || { echo "FAIL: build schema-user and schema-ctl first"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state XDG_CACHE_HOME=$T/cache XDG_CONFIG_HOME=$T/cfg SCHEMA_USER_IMPORT=
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID=
cleanup() { [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null; pkill -KILL -f "$T/" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; ./schema-ctl --user status 2>&1 | tail -12; tail -12 "$T/out"; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 80 ] && return 1; sleep 0.1; done; }
n() { [ -f "$T/$1.n" ] && wc -l < "$T/$1.n" | tr -d ' ' || echo 0; }
st() { ./schema-ctl --user status "$1" 2>&1; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"; mkdir -p "$S" "$T/cfg"; chmod 700 "$T/run/schema-user" "$S"
svc() {  # name, watch lines, shell body
    printf 'name=%s\nexec=/bin/sh\nargs=-c\nargs=echo >> %s/%s.n; %s\noneshot=1\n%b' "$1" "$T" "$1" "$3" "$2" > "$S/$1.svc"
}
printf x > "$T/conf"; : > "$T/s"
svc ex "watch_path=exists:$T/flag\n" "rm -f $T/flag"
svc dne "watch_path=dir_not_empty:$T/q\nmake_directory=1\ndirectory_mode=0700\n" "rm -f $T/q/*"
svc ch "watch_path=changed:$T/conf\n" ":"
svc slow "watch_path=changed:$T/s\n" "sleep 2"
svc selfloop "watch_path=changed:$T/loop\ntrigger_limit_burst=5\ntrigger_limit_interval_sec=30\n" "echo x >> $T/loop"
svc ex2 "watch_path=exists:$T/bm/flag\n" "rm -f $T/bm/flag"
# already true at arm: runs at once
: > "$T/pre"
svc pre "watch_path=exists:$T/pre\n" "rm -f $T/pre"
mkdir -p "$T/rp"; : > "$T/rp/other"
printf 'name=ready\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/ready.pid; touch %s/rp/ready; exec sleep 600\nready_path=%s/rp/ready\n' "$T" "$T" "$T" > "$S/ready.svc"
svc ch2 "watch_path=changed:$T/rp/other\n" ":"
chmod 600 "$S"/*.svc

./schema-user > "$T/out" 2>&1 &
PID=$!
until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"
until_ok "[ -s $T/ready.pid ]" || fail "ready never started"
sleep 0.5
for s in ex dne ch slow selfloop ex2 ch2; do [ "$(n $s)" = 0 ] || fail "$s ran at arm"; done
st ex | grep -q "watch_path=exists:$T/flag" || fail "status lacks the watch: $(st ex)"
until_ok '[ "$(n pre)" = 1 ]' || fail "pre: a level watch true at arm ran $(n pre) times"

# exists: fires, the service clears it, fires again on the next one
touch "$T/flag"
until_ok '[ "$(n ex)" = 1 ]' || fail "ex: $(n ex) runs after touch"
until_ok '[ ! -e "$T/flag" ]' || fail "ex never removed the flag"
touch "$T/flag"
until_ok '[ "$(n ex)" = 2 ]' || fail "ex: second touch, $(n ex) runs"

# dir_not_empty + make_directory
[ "$(stat -c %a "$T/q")" = 700 ] || fail "q is $(stat -c %a "$T/q" 2>&1)"
: > "$T/q/job"
until_ok '[ "$(n dne)" = 1 ] && [ ! -e "$T/q/job" ]' || fail "dne: $(n dne) runs"
sleep 0.5; [ "$(n dne)" = 1 ] || fail "dne ran again on an empty dir: $(n dne)"

# changed: one write, one run
printf y > "$T/conf"
until_ok '[ "$(n ch)" = 1 ]' || fail "ch: $(n ch) runs"
sleep 1; [ "$(n ch)" = 1 ] || fail "ch: one write ran it $(n ch) times"

# events while running coalesce into one more run
printf 1 > "$T/s"
until_ok '[ "$(n slow)" = 1 ]' || fail "slow never ran"
printf 2 > "$T/s"; printf 3 > "$T/s"
until_ok '[ "$(n slow)" = 2 ]' || fail "slow: no rerun ($(n slow))"
sleep 3; [ "$(n slow)" = 2 ] || fail "slow: $(n slow) runs, want 2"
# a reload while a rerun is pending keeps it
printf 4 > "$T/s"
until_ok '[ "$(n slow)" = 3 ]' || fail "slow: third run"
printf 5 > "$T/s"
sleep 0.3   # drained before the reload, so it is the reload that must carry it
./schema-ctl --user reload > /dev/null 2>&1 || fail "reload"
until_ok '[ "$(n slow)" = 4 ]' || fail "slow: rerun lost over a reload ($(n slow))"

# a service that writes its own watched path is parked
printf x > "$T/loop"
until_ok 'st selfloop | grep -q "excise_reason=trigger-limit-hit"' || fail "selfloop not parked: $(st selfloop)"
c=$(n selfloop); [ "$c" -le 7 ] || fail "selfloop ran $c times past a burst of 5"
rm -f "$T/loop"; sleep 0.5
st selfloop | grep -q "state=EXCISED" || fail "selfloop left the park on its own: $(st selfloop)"
./schema-ctl --user start selfloop > /dev/null 2>&1
# start clears the window: a fresh burst before it parks again
until_ok 'st selfloop | grep -q "excise_reason=trigger-limit-hit" && [ "$(n selfloop)" -ge $((c + 5)) ]' ||
    fail "start: $(n selfloop) runs after $c, window not cleared"

# a missing parent: the watch follows it into existence
mkdir -p "$T/bm"; touch "$T/bm/flag"
until_ok '[ "$(n ex2)" = 1 ]' || fail "ex2: $(n ex2) runs"

# a busy neighbour of a ready_path doesn't disturb the ready service
P=$(cat "$T/ready.pid")
i=0; while [ $i -lt 200 ]; do printf $i > "$T/rp/other"; i=$((i + 1)); done
sleep 1
[ "$(cat "$T/ready.pid")" = "$P" ] && kill -0 "$P" 2>/dev/null || fail "ready restarted"
st ready | grep -q "restarts=0" || fail "ready: $(st ready | head -1)"
[ "$(n ch2)" -ge 1 ] || fail "ch2 never ran"

# added at runtime with its condition already true: armed and fired at once
# (no backstop sweep is due here, unlike at boot)
: > "$T/late"
svc late "watch_path=exists:$T/late\n" "rm -f $T/late"
chmod 600 "$S/late.svc"
./schema-ctl --user add "$S/late.svc" > /dev/null 2>&1 || fail "add late"
until_ok '[ "$(n late)" = 1 ]' || fail "late: $(n late) runs after add"

echo "test_watch_path_live: ok"
