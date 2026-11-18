#!/bin/sh
# audit 4.9: randomized_delay_sec= and the next-fire status line, through the
# real engine (schema-user)
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_timer_jitter_live: skipped (root)"; exit 0; }
[ -x ./schema-user ] && [ -x ./schema-ctl ] || { echo "FAIL: build schema-user and schema-ctl first"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state XDG_CACHE_HOME=$T/cache XDG_CONFIG_HOME=$T/cfg SCHEMA_USER_IMPORT=
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID=
cleanup() { [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; ./schema-ctl --user status 2>&1 | tail -6; tail -8 "$T/out"; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge "${2:-80}" ] && return 1; sleep 0.1; done; }
st() { ./schema-ctl --user status "$1" 2>&1; }
nextfire() { st "$1" | sed -n 's/^  next fire: //p'; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"; mkdir -p "$S" "$T/cfg"; chmod 700 "$T/run/schema-user" "$S"
printf 'name=j\nexec=/bin/sh\nargs=-c\nargs=date +%%s.%%N >> %s/j.t\non_boot_sec=0\non_active_sec=1\nrandomized_delay_sec=2\n' "$T" > "$S/j.svc"
printf 'name=c\nexec=/bin/true\non_calendar=03:00\nrandomized_delay_sec=600\n' > "$S/c.svc"
printf 'name=i\nexec=/bin/true\non_boot_sec=3600\n' > "$S/i.svc"
for k in 1 2 3 4 5; do printf 'name=c%s\nexec=/bin/true\non_calendar=03:00\nrandomized_delay_sec=600\n' $k > "$S/c$k.svc"; done
chmod 600 "$S"/*.svc

./schema-user > "$T/out" 2>&1 &
PID=$!
until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"

# a calendar timer's delay: 03:00 plus at most 600 s, shown as local time
c1=$(nextfire c)
echo "$c1" | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2} 03:(0[0-9]|10):[0-9]{2}$' || fail "c next fire: '$c1'"
# six identical calendar timers don't all draw the same delay
[ "$(for k in "" 1 2 3 4 5; do nextfire c$k; done | sort -u | wc -l)" -gt 1 ] || fail "every 03:00 timer drew the same delay"
# an interval timer's monotonic deadline, shown as wall clock about an hour out
want=$(($(date +%s) + 3600))
got=$(date -d "$(nextfire i)" +%s 2>/dev/null) || fail "i next fire: '$(nextfire i)'"
[ $((got - want)) -le 5 ] && [ $((want - got)) -le 5 ] || fail "i fires at $got, want ~$want"
# a reload keeps a carried deadline: no fresh draw
./schema-ctl --user reload > /dev/null 2>&1 || fail "reload"
[ "$(nextfire c)" = "$c1" ] || fail "reload re-drew c: '$c1' -> '$(nextfire c)'"

# interval gaps: 1 s after each run plus 0..2 s, never less, never more
until_ok '[ "$(cat "$T/j.t" 2>/dev/null | wc -l)" -ge 9 ]' 400 || fail "j ran $(wc -l < "$T/j.t") times"
awk 'NR > 1 { g = $1 - p; if (g < 0.95 || g > 3.5) { print "gap " g; bad = 1 }
              if (NR == 2) { lo = hi = g } if (g < lo) lo = g; if (g > hi) hi = g }
     { p = $1 } END { if (hi - lo < 0.5) { print "gaps all alike " lo "-" hi; bad = 1 } exit bad }' "$T/j.t" ||
    fail "j gaps: $(tr '\n' ' ' < "$T/j.t")"

echo "test_timer_jitter_live: ok"
