#!/bin/sh
# A card past CARD_TIMEOUT_SECS is killed without PID 1 waiting on it (audit
# 2026-11-12, 0.2): a blocking waitpid on a card stuck in D state starved the
# watchdog. CI can't make a card outlive SIGKILL, so this checks the
# non-blocking path end to end: kill logged once, reap() collects it, no
# second card-failed line, the main process starts after, and the loop
# keeps answering. Built with a 2s timeout: $1 is that schema-user.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_card_timeout: skipped (root)"; exit 0; }
SU=${1:?usage: test_card_timeout.sh <schema-user built with -DCARD_TIMEOUT_SECS=2>}
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state SCHEMA_USER_IMPORT=
PID=
cleanup() { [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null; pkill -KILL -f "$T/" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; tail -20 "$T/out"; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 100 ] && return 1; sleep 0.1; done; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"
mkdir -p "$S"
chmod 700 "$T/run/schema-user" "$S"
# first run fails so the retry plays the card; later runs stay up
printf '#!/bin/sh\n[ -e %s/ran ] && exec sleep 30\ntouch %s/ran\nexit 1\n' "$T" "$T" > "$T/main"
ln -s "$(command -v sleep)" "$T/hang"
chmod 755 "$T/main"
printf 'name=ct\nexec=%s/main\ncard=exec:%s/hang 60\n' "$T" "$T" > "$S/ct.svc"
chmod 600 "$S/ct.svc"
"$SU" > "$T/out" 2>&1 &
PID=$!
until_ok 'grep -q " ct .*card-timeout" "$T/out"' || fail "card never timed out"
until_ok '! pgrep -f "$T/hang" > /dev/null' || fail "card still alive after its timeout"
until_ok './schema-ctl --user status 2>&1 | grep -Eq "^ +ct .*pid=[1-9]"' || fail "main never started after the card"
[ "$(grep -c ' ct .*card-timeout' "$T/out")" = 1 ] || fail "card-timeout logged more than once"
grep -q ' ct .*card-failed' "$T/out" && fail "killed card also logged card-failed"
echo "test_card_timeout: ok"
