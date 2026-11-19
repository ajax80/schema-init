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
# no shell in between: dash forks the command and reports the child's
# SIGSYS as exit 159, bash execs it; the service itself must be the one killed
printf 'name=kill\nexec=/usr/bin/uname\nargs=-s\noneshot=1\nsyscall_filter=~uname\nsyscall_error_number=kill\n' > "$S/kill.svc"
svc suid "touch $T/f; chmod 4755 $T/f; echo \$? > $T/suid.rc" "restrict_suid_sgid=1\n"
printf 'name=stopper\nexec=/bin/sleep\nargs=600\nsyscall_filter=~uname\nexec_stop=/bin/sh -c "uname -s > %s/st 2>/dev/null || echo blocked > %s/st"\n' "$T" "$T" > "$S/stopper.svc"
printf 'name=reloader\nexec=/bin/sleep\nargs=600\nsyscall_filter=~uname\nexec_reload=/bin/sh -c "uname -s > %s/rl 2>/dev/null || echo blocked > %s/rl"\n' "$T" "$T" > "$S/reloader.svc"
# the runner itself is never filtered: its waitpid works even under ~wait4,
# so the second stop command waits for the first
printf 'name=waiter\nexec=/bin/sleep\nargs=600\nsyscall_filter=~wait4 waitid\nexec_stop=/bin/sleep 2\nexec_stop=/usr/bin/touch %s/second\n' "$T" > "$S/waiter.svc"
# plain exec_pre lines are filtered, + lines are not
printf 'name=pre\nexec=/bin/true\noneshot=1\nsyscall_filter=~uname\nexec_pre=/bin/sh -c "uname -s > %s/pre.out 2>/dev/null || echo blocked > %s/pre.out"\nexec_pre=+/bin/sh -c "uname -s > %s/plus.out"\n' "$T" "$T" "$T" > "$S/pre.svc"
printf 'name=lp\nexec=/bin/sleep\nargs=600\nlock_personality=1\n' > "$S/lp.svc"
# syscall_deny=: an unprivileged clock_settime is EPERM anyway, so kill makes the
# preset visible (SIGSYS); syscall_deny=0 leaves date to fail on its own
printf 'name=sdd\nexec=/usr/bin/date\nargs=-s\nargs=@0\noneshot=1\nsyscall_deny=default\nsyscall_error_number=kill\n' > "$S/sdd.svc"
printf 'name=sd0\nexec=/usr/bin/date\nargs=-s\nargs=@0\noneshot=1\nsyscall_deny=0\nsyscall_error_number=kill\n' > "$S/sd0.svc"
chmod 600 "$S"/*.svc

./schema-user > "$T/out" 2>&1 &
PID=$!
until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"

until_ok "[ -s $T/ok.out ]" || fail "@system-service service never ran"
until_ok "[ -s $T/deny.rc ]" || fail "deny never ran"
[ "$(cat "$T/deny.rc")" != 0 ] || fail "uname ran under ~uname: $(cat "$T/deny.out")"
until_ok 'st kill | grep -q SIGSYS' || fail "kill: no SIGSYS: $(st kill | head -3)"
until_ok "[ -s $T/suid.rc ]" || fail "suid never ran"
[ "$(cat "$T/suid.rc")" != 0 ] || fail "chmod 4755 went through restrict_suid_sgid"
st ok | grep -q "syscall_filter: allowlist" || fail "status: $(st ok)"
# the stop runner is filtered too
until_ok 'st stopper | grep -q "pid=[1-9]"' || fail "stopper never started"
./schema-ctl --user stop stopper > /dev/null 2>&1
until_ok "[ -s $T/st ]" || fail "exec_stop never ran"
[ "$(cat "$T/st")" = blocked ] || fail "exec_stop ran unfiltered: $(cat "$T/st")"

# exec_reload shares the stop runner; pin it separately
until_ok 'st reloader | grep -q "pid=[1-9]"' || fail "reloader never started"
./schema-ctl --user reload reloader > /dev/null 2>&1
until_ok "[ -s $T/rl ]" || fail "exec_reload never ran"
[ "$(cat "$T/rl")" = blocked ] || fail "exec_reload ran unfiltered: $(cat "$T/rl")"

until_ok 'st waiter | grep -q "pid=[1-9]"' || fail "waiter never started"
t0=$(date +%s)
./schema-ctl --user stop waiter > /dev/null 2>&1
until_ok "[ -e $T/second ]" || fail "waiter: second exec_stop never ran"
[ $(($(stat -c %Y "$T/second") - t0)) -ge 2 ] || fail "waiter: second exec_stop did not wait for the first"
until_ok "[ -s $T/pre.out ] && [ -s $T/plus.out ]" || fail "pre: exec_pre lines never ran"
[ "$(cat "$T/pre.out")" = blocked ] || fail "plain exec_pre ran unfiltered: $(cat "$T/pre.out")"
[ "$(cat "$T/plus.out")" = Linux ] || fail "+ exec_pre was filtered"
st lp | grep -q "native-arch" || fail "status hides the arch check: $(st lp | grep seccomp)"
until_ok 'st sdd | grep -q SIGSYS' || fail "syscall_deny=default: no SIGSYS from clock_settime: $(st sdd | head -3)"
st sdd | grep -Eq "syscall_deny +default +explicit" || fail "status: $(st sdd | grep syscall_deny)"
until_ok 'st sd0 | grep -Eq "exit|code|status=[0-9]"' || fail "sd0 never ran"
st sd0 | grep -q SIGSYS && fail "syscall_deny=0 still filtered: $(st sd0 | head -3)"

echo "test_seccomp_live: ok"
