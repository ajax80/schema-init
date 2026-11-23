#!/bin/sh
# schema-ctl log [-f] [-n N] <name>, unprivileged: both system log dirs aimed at
# temp dirs through the testhooks build's SCHEMA_CTL_LOG_VAR / SCHEMA_CTL_LOG_RUN.
set -u
cd "$(dirname "$0")/.."
CTL=${1:-build/test/schema-ctl-testhooks}
[ -x "$CTL" ] || { echo "FAIL ctl_log: no $CTL"; exit 1; }
T=$(mktemp -d)
trap 'kill $FP 2>/dev/null; chmod -R u+rwx "$T" 2>/dev/null; rm -rf "$T"' EXIT
FP=
fail() { echo "FAIL ctl_log: $*"; [ -f "$T/err" ] && sed 's/^/  | /' "$T/err"; exit 1; }
V=$T/var R=$T/run
mkdir -p "$V" "$R"
export SCHEMA_CTL_LOG_VAR="$V" SCHEMA_CTL_LOG_RUN="$R"
lg() { "$CTL" log "$@" > "$T/out" 2> "$T/err"; rc=$?; }

seq 1 120 | sed 's/^/line /' > "$V/a.log"
lg a
[ "$rc" -eq 0 ] || fail "a: exit $rc"
[ "$(seq 71 120 | sed 's/^/line /')" = "$(cat "$T/out")" ] || fail "default tail is not lines 71-120"
lg -n 5 a
[ "$(seq 116 120 | sed 's/^/line /')" = "$(cat "$T/out")" ] || fail "-n 5"
lg -n 0 a
[ "$(wc -l < "$T/out")" -eq 120 ] || fail "-n 0 is not the whole file"
# longer than one 4 KB chunk per line
awk 'BEGIN { for (i = 1; i <= 30; i++) { s = ""; for (j = 0; j < 300; j++) s = s "x"; print i " " s } }' > "$V/big.log"
lg -n 3 big
[ "$(cut -d' ' -f1 "$T/out" | tr '\n' ' ')" = "28 29 30 " ] || fail "-n 3 across chunks: $(cut -c1-8 "$T/out")"

printf 'one\ntwo' > "$V/nonl.log"
lg nonl
[ "$(cat "$T/out")" = "$(printf 'one\ntwo')" ] || fail "no trailing newline"

echo run-only > "$R/b.log"
lg b
[ "$(cat "$T/out")" = run-only ] || fail "/run fallback"
echo old > "$V/c.log"; touch -d '1 hour ago' "$V/c.log"; echo new > "$R/c.log"
lg c
[ "$(cat "$T/out")" = new ] || fail "newer of /var and /run"

lg nope
[ "$rc" -eq 1 ] || fail "nope: exit $rc"
grep -q "no log for nope" "$T/err" || fail "nope: message"
if [ "$(id -u)" != 0 ]; then
    echo secret > "$V/d.log"; chmod 000 "$V/d.log"
    lg d
    [ "$rc" -eq 1 ] && grep -q "needs root: sudo schema-ctl log d" "$T/err" || fail "EACCES: exit $rc"
fi
lg
[ "$rc" -eq 64 ] || fail "no name: exit $rc"

# follow across a logrotate-style rename, the new file appearing a second later
echo zero > "$V/e.log"
"$CTL" log -n 0 -f e > "$T/fout" 2>&1 &
FP=$!
sleep 0.5
echo one >> "$V/e.log"
sleep 0.3
mv "$V/e.log" "$V/e.log.1"
sleep 1
echo two > "$V/e.log"
sleep 0.3
echo three >> "$V/e.log"
i=0
while [ $i -lt 30 ] && ! grep -q three "$T/fout"; do sleep 0.1; i=$((i + 1)); done
for w in zero one two three; do grep -qx "$w" "$T/fout" || { cat "$T/fout"; fail "follow: $w missing"; }; done
kill $FP; FP=

mkdir -p "$T/state/schema-user/log"
echo mine > "$T/state/schema-user/log/f.log"
# no XDG_RUNTIME_DIR (ssh, CI): log needs only the state dir
env -u XDG_RUNTIME_DIR XDG_STATE_HOME="$T/state" "$CTL" --user log f > "$T/out" 2> "$T/err"
[ "$(cat "$T/out")" = mine ] || fail "--user log"

"$CTL" > /dev/null 2> "$T/usage"
grep -q "rail's log" "$T/usage" || fail "usage: the log entry is cut short"
echo "ctl_log: ok"
