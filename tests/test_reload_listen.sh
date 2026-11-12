#!/bin/sh
# A reload must act on a changed or removed listen= (audit 2026-11-12, 0.3):
# the old socket used to be carried until the next boot.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_reload_listen: skipped (root)"; exit 0; }
[ -x ./schema-user ] || { echo "FAIL: no ./schema-user"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state SCHEMA_USER_IMPORT=
PID=
cleanup() { [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null; pkill -KILL -f "$T/sl" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; tail -20 "$T/out"; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 50 ] && return 1; sleep 0.1; done; }
# listening as long as some fd holds it; never connect: that would wake the lazy service
listening() { awk -v p="$1" '$NF == p { f = 1 } END { exit !f }' /proc/net/unix; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"
mkdir -p "$S"
chmod 700 "$T/run/schema-user" "$S"
ln -s "$(command -v sleep)" "$T/sl"
svc() { printf 'name=ls\nexec=%s\nargs=30\nlisten_lazy=1\n%s' "$T/sl" "$1" > "$S/ls.svc"; chmod 600 "$S/ls.svc"; }
svc "listen=stream:$T/a.sock
"
./schema-user > "$T/out" 2>&1 &
PID=$!
st() { ./schema-ctl --user status 2>&1 | grep -E '^ +ls '; }
until_ok 'st | grep -q "state=PERFECT"' || fail "ls never parked on its socket"
listening "$T/a.sock" || fail "a.sock not listening"

svc "listen=stream:$T/b.sock
"
./schema-ctl --user reload > /dev/null || fail "reload (changed listen)"
until_ok 'listening "$T/b.sock"' || fail "changed listen=: b.sock not bound after reload"
listening "$T/a.sock" && fail "changed listen=: a.sock still held after reload"
st | grep -q "pid=0 " || fail "changed listen=: the reload started the parked service: $(st)"

svc ""
./schema-ctl --user reload > /dev/null || fail "reload (listen removed)"
listening "$T/b.sock" && fail "removed listen=: b.sock still held after reload"
until_ok 'st | grep -Eq "pid=[1-9]"' || fail "removed listen=: parked service never started: $(st)"
echo "test_reload_listen: ok"
