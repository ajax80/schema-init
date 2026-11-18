#!/bin/sh
# audit 4.6: seccomp through the real engine (schema-user: no_new_privs +
# filter needs no root)
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_seccomp_live: skipped (root)"; exit 0; }
[ -x ./schema-user ] && [ -x ./schema-ctl ] || { echo "FAIL: build schema-user and schema-ctl first"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state XDG_CACHE_HOME=$T/cache XDG_CONFIG_HOME=$T/cfg SCHEMA_USER_IMPORT=
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID=
cleanup() { [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; ./schema-ctl --user status 2>&1 | tail -8; tail -8 "$T/out"; exit 1; }
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 80 ] && return 1; sleep 0.1; done; }
st() { ./schema-ctl --user status "$1" 2>&1; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"; mkdir -p "$S" "$T/cfg"; chmod 700 "$T/run/schema-user" "$S"
svc() {  # name, body, extra lines
    printf 'name=%s\nexec=/bin/sh\nargs=-c\nargs=%s\noneshot=1\n%b' "$1" "$2" "$3" > "$S/$1.svc"
}
svc ok "echo ok > $T/ok.out" "syscall_filter=@system-service\nsyscall_filter=~@privileged @resources\n"
svc deny "uname -s > $T/deny.out 2>&1; echo \$? > $T/deny.rc" "syscall_filter=~uname\n"
svc kill "uname -s > $T/kill.out" "syscall_filter=~uname\nsyscall_error_number=kill\n"
svc suid "touch $T/f; chmod 4755 $T/f; echo \$? > $T/suid.rc" "restrict_suid_sgid=1\n"
printf 'name=stopper\nexec=/bin/sleep\nargs=600\nsyscall_filter=~uname\nexec_stop=/bin/sh -c "uname -s > %s/st 2>/dev/null || echo blocked > %s/st"\n' "$T" "$T" > "$S/stopper.svc"
chmod 600 "$S"/*.svc

./schema-user > "$T/out" 2>&1 &
PID=$!
until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"

until_ok "[ -s $T/ok.out ]" || fail "@system-service service never ran"
until_ok "[ -s $T/deny.rc ]" || fail "deny never ran"
[ "$(cat "$T/deny.rc")" != 0 ] || fail "uname ran under ~uname: $(cat "$T/deny.out")"
until_ok 'st kill | grep -q SIGSYS' || fail "kill: no SIGSYS: $(st kill | head -3)"
[ ! -s "$T/kill.out" ] || fail "kill: uname printed"
until_ok "[ -s $T/suid.rc ]" || fail "suid never ran"
[ "$(cat "$T/suid.rc")" != 0 ] || fail "chmod 4755 went through restrict_suid_sgid"
st ok | grep -q "syscall_filter: allowlist" || fail "status: $(st ok)"
# the stop runner is filtered too
until_ok 'st stopper | grep -q "pid=[1-9]"' || fail "stopper never started"
./schema-ctl --user stop stopper > /dev/null 2>&1
until_ok "[ -s $T/st ]" || fail "exec_stop never ran"
[ "$(cat "$T/st")" = blocked ] || fail "exec_stop ran unfiltered: $(cat "$T/st")"

echo "test_seccomp_live: ok"
