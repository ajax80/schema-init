#!/bin/sh
# audit 4.4: runtime_dir=/state_dir=/cache_dir=/logs_dir= through the real
# engine (schema-user: the XDG bases; ownership is covered on PID 1)
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_exec_dirs_live: skipped (root)"; exit 0; }
[ -x ./schema-user ] && [ -x ./schema-ctl ] || { echo "FAIL: build schema-user and schema-ctl first"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state XDG_CACHE_HOME=$T/cache XDG_CONFIG_HOME=$T/cfg SCHEMA_USER_IMPORT=
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID=
cleanup() { [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null; pkill -KILL -f "$T/" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; ./schema-ctl --user status 2>&1 | tail -8; tail -8 "$T/out"; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 80 ] && return 1; sleep 0.1; done; }
line() { ./schema-ctl --user status 2>&1 | grep -E "^ +$1 "; }
mode() { stat -c %a "$1"; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"; mkdir -p "$S" "$T/cfg" "$T/outside"; chmod 700 "$T/run/schema-user" "$S"
ln -s "$T/outside" "$T/run/trap"
printf 'name=rd\nexec=/bin/sh\nargs=-c\nargs=env | grep _DIRECTORY= | sort > %s/rd.env; echo $$ > %s/rd.pid; while :; do sleep 1 & wait; done\nruntime_dir=rd/nested rd2\nruntime_dir_mode=0700\nstate_dir=st\ncache_dir=ca\nlogs_dir=lg\nlogs_dir_mode=0750\nenv=CACHE_DIRECTORY=/mine\nexec_stop=/bin/sh -c "echo $RUNTIME_DIRECTORY > %s/stop.env"\nstable_secs=1\n' "$T" "$T" "$T" > "$S/rd.svc"
# a symlink planted in a parent: never followed, the spawn fails
printf 'name=evil\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/evil.pid; exec sleep 600\nruntime_dir=trap/x\nstable_secs=1\n' "$T" > "$S/evil.svc"
chmod 600 "$S"/*.svc

./schema-user > "$T/out" 2>&1 &
PID=$!
until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"
until_ok "[ -s $T/rd.pid ]" || fail "rd never started"

[ -d "$T/run/rd/nested" ] && [ "$(mode "$T/run/rd/nested")" = 700 ] || fail "runtime_dir rd/nested: $(ls -la "$T/run/rd" 2>&1)"
[ "$(mode "$T/run/rd")" = 755 ] || fail "intermediate rd is $(mode "$T/run/rd")"
[ "$(mode "$T/run/rd2")" = 700 ] || fail "rd2"
[ "$(mode "$T/state/st")" = 755 ] || fail "state_dir"
[ -d "$T/cache/ca" ] || fail "cache_dir"
[ "$(mode "$T/state/log/lg")" = 750 ] || fail "logs_dir"
grep -qx "RUNTIME_DIRECTORY=$T/run/rd/nested:$T/run/rd2" "$T/rd.env" || fail "RUNTIME_DIRECTORY: $(cat "$T/rd.env")"
grep -qx "STATE_DIRECTORY=$T/state/st" "$T/rd.env" || fail "STATE_DIRECTORY"
grep -qx "LOGS_DIRECTORY=$T/state/log/lg" "$T/rd.env" || fail "LOGS_DIRECTORY"
grep -qx "CACHE_DIRECTORY=/mine" "$T/rd.env" || fail "env= must win: $(cat "$T/rd.env")"

until_ok "grep -qs 'evil: runtime_dir=trap/x' $T/state/schema-user/log/evil.log" || fail "evil: no runtime_dir error logged: $(ls $T/state/schema-user/log)"
[ ! -e "$T/outside/x" ] || fail "followed the planted symlink"
[ ! -s "$T/evil.pid" ] || fail "evil ran"

# a crash respawn keeps the runtime dir
touch "$T/run/rd2/keep"
P1=$(cat "$T/rd.pid"); : > "$T/rd.pid"
kill -KILL "$P1"
until_ok "[ -s $T/rd.pid ]" || fail "rd never respawned"
[ -e "$T/run/rd2/keep" ] || fail "runtime dir lost on a respawn"

# a stop removes the runtime dirs, keeps the rest
./schema-ctl --user stop rd > /dev/null || fail "stop rd"
until_ok "line rd | grep -q state=EXCISED" || fail "rd: $(line rd)"
grep -qx "$T/run/rd/nested:$T/run/rd2" "$T/stop.env" || fail "exec_stop: RUNTIME_DIRECTORY=$(cat "$T/stop.env" 2>&1)"
until_ok "[ ! -e $T/run/rd2 ] && [ ! -e $T/run/rd/nested ]" || fail "runtime dirs left after stop: $(ls -a "$T/run")"
[ -z "$(ls -A "$T/run" | grep schema-gone)" ] || until_ok "[ -z \"\$(ls -A $T/run | grep schema-gone)\" ]" || fail "aside dir never deleted"
[ -d "$T/state/st" ] && [ -d "$T/state/log/lg" ] || fail "state/logs dirs removed on stop"

# started again: a fresh runtime dir
: > "$T/rd.pid"
./schema-ctl --user start rd > /dev/null || fail "start rd"
until_ok "[ -s $T/rd.pid ]" || fail "rd never restarted"
[ -d "$T/run/rd2" ] && [ ! -e "$T/run/rd2/keep" ] || fail "restart after stop: $(ls -a "$T/run/rd2" 2>&1)"
echo "test_exec_dirs_live: ok"
