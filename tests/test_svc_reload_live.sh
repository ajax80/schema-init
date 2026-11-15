#!/bin/sh
# audit 4.1: schema-ctl reload <svc> through the real engine (schema-user runs
# the same engine.c as PID 1, unprivileged)
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_svc_reload_live: skipped (root)"; exit 0; }
[ -x ./schema-user ] && [ -x ./schema-ctl ] || { echo "FAIL: build schema-user and schema-ctl first"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state XDG_CONFIG_HOME=$T/cfg SCHEMA_USER_IMPORT=
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID=
cleanup() { [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null; pkill -KILL -f "$T/" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; tail -20 "$T/out"; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 50 ] && return 1; sleep 0.1; done; }
ctl() { ./schema-ctl --user "$@" 2>&1; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"; mkdir -p "$S" "$T/cfg"; chmod 700 "$T/run/schema-user" "$S"
# main writes its pid, counts HUPs; TERM exits
main() {
    printf 'name=%s\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/%s.pid; trap "echo h >> %s/%s.hup" HUP; trap "exit 0" TERM; while :; do sleep 1 & wait; done\nstable_secs=1\n' \
        "$1" "$T" "$1" "$T" "$1"
}
{ main sig; echo reload_signal=HUP; } > "$S/sig.svc"
{ main old; echo reload=1; } > "$S/old.svc"
{ main cmd; printf 'exec_reload=/bin/sh -c "echo $MAINPID-${MAINPID} > %s/cmd.mp"\n' "$T"; } > "$S/cmd.svc"
# the runner sleeps as $T/sl so cleanup's pkill finds it
{ main hang; printf 'exec_reload=%s/sl 100\nstop_timeout_sec=2\n' "$T"; } > "$S/hang.svc"
{ main bad; echo exec_reload=/bin/false; } > "$S/bad.svc"
main none > "$S/none.svc"
{ main off; echo reload_signal=HUP; } > "$S/off.svc"
chmod 600 "$S"/*.svc
ln -s "$(command -v sleep)" "$T/sl"

./schema-user > "$T/out" 2>&1 &
PID=$!
until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"
for s in sig old cmd hang bad none off; do until_ok "[ -s $T/$s.pid ]" || fail "$s never started"; done
ctl stop off > /dev/null

r=$(ctl reload sig) || fail "reload sig: $r"
case "$r" in "ok: SIGHUP → sig (pid $(cat "$T/sig.pid"))") ;; *) fail "reload sig reply: $r" ;; esac
until_ok "[ -s $T/sig.hup ]" || fail "sig never got SIGHUP"
r=$(ctl reload old) || fail "reload=1 service: $r"
until_ok "[ -s $T/old.hup ]" || fail "reload=1 did not send SIGHUP"

r=$(ctl reload cmd) || fail "reload cmd: $r"
case "$r" in "ok: exec_reload → cmd"*) ;; *) fail "reload cmd reply: $r" ;; esac
P=$(cat "$T/cmd.pid")
until_ok "[ -s $T/cmd.mp ]" || fail "exec_reload never ran"
# $MAINPID and ${MAINPID} are both the pid
grep -qx "$P-$P" "$T/cmd.mp" || fail "MAINPID: $(cat "$T/cmd.mp") want $P"

r=$(ctl reload hang) || fail "reload hang: $r"
r=$(ctl reload hang) && fail "second reload while one runs: $r"
case "$r" in *"already reloading"*) ;; *) fail "busy reply: $r" ;; esac
sleep 2
until_ok 'ctl reload hang > /dev/null' || fail "runner not killed after stop_timeout_sec"

r=$(ctl reload none) && fail "reload without a key succeeded: $r"
case "$r" in "err: cannot-reload: none"*) ;; *) fail "cannot-reload reply: $r" ;; esac
r=$(ctl reload off) && fail "reload of a stopped service: $r"
case "$r" in *"is not running"*) ;; *) fail "not running reply: $r" ;; esac
r=$(ctl reload nosuch) && fail "reload of a missing service: $r"
case "$r" in *"not found"*) ;; *) fail "not found reply: $r" ;; esac
r=$(ctl reload bad) || fail "reload bad: $r"

# a reload is not a restart: same pids, no restarts
st=$(ctl status)
for s in sig old cmd hang bad; do
    echo "$st" | grep -Eq "^ +$s +pid=$(cat "$T/$s.pid") .*restarts=0" || fail "$s pid or restarts changed: $(echo "$st" | grep " $s ")"
done
# bare reload still re-reads the .svc files
r=$(ctl reload) || fail "bare reload: $r"
case "$r" in "ok: reload (graceful)"*) ;; *) fail "bare reload reply: $r" ;; esac
# extra spaces before --evict are still the config reload, not a service named --evict
r=$(python3 -c 'import socket,sys
c=socket.socket(socket.AF_UNIX); c.connect(sys.argv[1]); c.sendall(b"reload  --evict\n")
print(c.makefile().readline().strip())' "$T/run/schema-user/ctl.sock")
case "$r" in "ok: reload (evict mode)"*) ;; *) fail "reload  --evict: $r" ;; esac
echo "test_svc_reload_live: ok"
