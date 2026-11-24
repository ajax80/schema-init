#!/bin/sh
# schema-user re-execs in place: same pid, its services kept.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_schema_user_reexec: skipped (root)"; exit 0; }
[ -x ./schema-user ] || { echo "FAIL: no ./schema-user"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state XDG_CONFIG_HOME=$T/cfg
export SCHEMA_USER_IMPORT=
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID=
cleanup() {
    [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null
    pkill -KILL -f "$T/" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; [ -f "$T/out" ] && tail -20 "$T/out"; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 100 ] && return 1; sleep 0.1; done; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"
mkdir -p "$S" "$T/cfg"
chmod 700 "$T/run/schema-user" "$S"
cp ./schema-user "$T/su-old"
cp ./schema-user "$T/su-new"
printf '#!/bin/sh\necho "err: nope"\nexit 1\n' > "$T/bad"
chmod +x "$T/bad"

st() { ./schema-ctl --user status 2>&1; }
rx() { ./schema-ctl --user reexec "$@" 2>&1; }
pid_of() { st | sed -n "s/^ *$1 *pid=\([0-9]*\) .*/\1/p"; }
start() {
    "$T/su-old" > "$T/out" 2>&1 &
    PID=$!
    until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"
}
stop() {
    kill -TERM "$PID"
    until_ok '! kill -0 '"$PID"' 2>/dev/null' || fail "schema-user $PID did not stop"
    wait "$PID"
    PID=
}

# zero services: a user with nothing enabled re-execs too
start
r=$(rx "$T/su-new")
echo "$r" | grep -Eq "^ok: re-executed into .*, 0 services adopted, 0 new" || fail "zero services: $r"
stop

printf 'name=a\nexec=/bin/sh\nargs=-c\nargs=trap "date +%%s%%N > %s/a.stop; exit 0" TERM; while :; do sleep 1 & wait; done\nstable_secs=1\n' \
    "$T" > "$S/a.svc"
chmod 600 "$S/a.svc"
start
until_ok 'st | grep -Eq "^ +a +pid=[1-9][0-9]* +state=FUNDAMENTAL"' || { st; fail "a not FUNDAMENTAL"; }
A=$(pid_of a)
CG=$(cat "/proc/$A/cgroup")

# adopt: same schema-user pid, same service pid and cgroup, new binary, argv[0] kept
r=$(rx "$T/su-new")
echo "$r" | grep -Eq "^ok: re-executed into .*, 1 services adopted, 0 new" || fail "adopt: $r"
kill -0 "$PID" || fail "schema-user gone after re-exec"
[ "$(readlink "/proc/$PID/exe")" = "$T/su-new" ] || fail "exe is $(readlink "/proc/$PID/exe")"
[ "$(tr '\0' '\n' < "/proc/$PID/cmdline" | head -1)" = "$T/su-old" ] || fail "argv[0] changed: $(tr '\0' ' ' < "/proc/$PID/cmdline")"
[ "$(pid_of a)" = "$A" ] || fail "a pid $A -> $(pid_of a)"
[ "$(cat "/proc/$A/cgroup")" = "$CG" ] || fail "a cgroup changed"
# still supervised by the new image
kill -KILL "$A"
until_ok '[ -n "$(pid_of a)" ] && [ "$(pid_of a)" != 0 ] && [ "$(pid_of a)" != '"$A"' ]' || { st; fail "a not respawned after re-exec"; }
A=$(pid_of a)

# a binary that fails its dry run: refused, nothing moves
r=$(rx "$T/bad")
echo "$r" | grep -q "^err:" || fail "bad binary accepted: $r"
kill -0 "$PID" || fail "schema-user gone after refused re-exec"
[ "$(readlink "/proc/$PID/exe")" = "$T/su-new" ] || fail "exe after refusal: $(readlink "/proc/$PID/exe")"
[ "$(pid_of a)" = "$A" ] || fail "a moved after refusal"

# a dependency cycle: refused at the dry run
printf 'name=x\nexec=/bin/true\noneshot=1\ndep=y\n' > "$S/x.svc"
printf 'name=y\nexec=/bin/true\noneshot=1\ndep=x\n' > "$S/y.svc"
r=$(rx "$T/su-new")
echo "$r" | grep -q "^err:.*cycle" || fail "cycle not refused: $r"
rm "$S/x.svc" "$S/y.svc"
[ "$(pid_of a)" = "$A" ] || fail "a moved after cycle refusal"

# again, from the re-exec'd image: the lock and the default path are carried
r=$(rx)
echo "$r" | grep -Eq "^ok: re-executed into .*, 1 services adopted" || fail "second re-exec: $r"
[ "$(readlink "/proc/$PID/exe")" = "$T/su-old" ] || fail "default path is not argv[0]: $(readlink "/proc/$PID/exe")"
"$T/su-new" > "$T/second" 2>&1 && fail "second instance started after re-exec"
grep -q "already running" "$T/second" || fail "second instance: $(cat "$T/second")"

# environment survives: a service added now still sees XDG_RUNTIME_DIR
printf 'name=envdump\nexec=/bin/sh\nargs=-c\nargs=env > %s/env\noneshot=1\n' "$T" > "$S/envdump.svc"
chmod 600 "$S/envdump.svc"
./schema-ctl --user add envdump > /dev/null || fail "add envdump"
until_ok '[ -s "$T/env" ]' || fail "envdump did not run"
grep -qx "XDG_RUNTIME_DIR=$T/run" "$T/env" || fail "XDG_RUNTIME_DIR lost: $(grep XDG_RUNTIME_DIR "$T/env")"

# SIGTERM still stops it in order (held signals unblocked after adopt)
kill -TERM "$PID"
until_ok '! kill -0 '"$PID"' 2>/dev/null' || fail "SIGTERM ignored after re-exec"
wait "$PID"; rc=$?
PID=
[ "$rc" = 0 ] || fail "exit $rc after SIGTERM"
[ -s "$T/a.stop" ] || fail "a never got SIGTERM"

echo "test_schema_user_reexec: ok"
