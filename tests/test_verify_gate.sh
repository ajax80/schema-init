#!/bin/sh
# The gates on schema-ctl reboot / poweroff / reexec, against stub verifiers.
# The control socket points at a path that does not exist, so "proceeded" shows
# as the connect error and nothing ever reaches a real PID 1.
set -u
cd "$(dirname "$0")/.."
TD=${TESTDIR:-$PWD/build/test}
# SCHEMA_VERIFY_BIN / SCHEMA_VERIFY_CTL_SOCK exist only in a -DSCHEMA_VERIFY_TESTHOOKS build
CTL=${1:-$TD/schema-ctl-testhooks}
case $CTL in /*) ;; *) CTL=$PWD/$CTL ;; esac
[ -x "$CTL" ] || { echo "FAIL verify_gate: no $CTL"; exit 1; }
T=$TD/vg
rm -rf "$T"
mkdir -p "$T"
trap '[ -n "${KEEP:-}" ] || rm -rf "$T"' EXIT
SOCK=$T/no-such.sock
export SCHEMA_VERIFY_CTL_SOCK="$SOCK"

fail() {
    echo "FAIL verify_gate: $*"
    [ -f "$T/out" ] && sed 's/^/  | /' "$T/out"
    exit 1
}

stub() { # name, body: each stub also leaves a mark that it ran
    printf '#!/bin/sh\n: > %s/ran\n%s\n' "$T" "$2" > "$T/$1"
    chmod 755 "$T/$1"
}
stub v0 'printf "Result: 0 fail, 0 change, 0 warn\n"; exit 0'
stub v1 'printf "[FAIL] next boot: init=/sbin/nope missing\nResult: 1 fail, 0 change, 0 warn\n"; exit 1'
stub v2 'printf "[CHANGE] a.exec: /bin/true -> /bin/false\nResult: 0 fail, 1 change (not accepted), 0 warn\n"; exit 2'
stub v3 'printf "[UNRESOLVED] Next boot: needs root\nResult: 0 fail, 0 change, 0 warn, 1 unresolved\n"; exit 3'
stub v64 'echo "usage: schema-ctl verify [--quiet] [--accept]" >&2; exit 64'
stub vsegv 'kill -SEGV $$'
stub vhang 'sleep 40'
stub vpid 'echo $$ > '"$T"'/pid; exec sleep 40'
stub v1err 'echo verify-said-on-stderr >&2; printf "[FAIL] x\nResult: 1 fail, 0 change, 0 warn\n"; exit 1'

# a pty on stdin/stdout/stderr: util-linux script, else python's openpty
PTY=
if command -v script >/dev/null 2>&1; then
    PTY=script
elif command -v python3 >/dev/null 2>&1; then
    PTY=python
else
    echo "verify_gate: no script(1) or python3 — the interactive cases are skipped"
fi
PTYPY='import os,subprocess,sys
m,s=os.openpty()
p=subprocess.Popen(sys.argv[1:],stdin=s,stdout=s,stderr=s,start_new_session=True)
os.close(s)
o=b""
while True:
    try: d=os.read(m,4096)
    except OSError: break
    if not d: break
    o+=d
sys.stdout.buffer.write(o)
r=p.wait()
sys.exit(r if r>=0 else 128-r)'

# tty <verifier> <args...>: run CTL with stdin+stderr on a pty; output in $T/out, status in $rc
tty() {
    v=$1; shift
    rm -f "$T/ran"
    if [ "$PTY" = script ]; then
        q="SCHEMA_VERIFY_BIN='$T/$v' '$CTL'"
        for a in "$@"; do q="$q '$a'"; done
        script -qec "$q" /dev/null < /dev/null > "$T/out" 2>&1
    else
        SCHEMA_VERIFY_BIN="$T/$v" python3 -c "$PTYPY" "$CTL" "$@" < /dev/null > "$T/out" 2>&1
    fi
    rc=$?
}
# notty <verifier> <args...>
notty() {
    v=$1; shift
    rm -f "$T/ran"
    SCHEMA_VERIFY_BIN="$T/$v" "$CTL" "$@" < /dev/null > "$T/out" 2>&1
    rc=$?
}
has() { grep -qF -- "$1" "$T/out"; }
connected() { has "$SOCK: No such file or directory"; }
refused_msg() { has "schema-ctl: $1 refused — fix the above or run 'schema-ctl verify --accept'; run 'schema-ctl $1 --force' to override"; }
failed_msg() { has "schema-ctl: $1 refused — fix the above; run 'schema-ctl $1 --force' to override" && ! has 'verify --accept'; }
killpid() { [ -s "$T/pid" ] && kill -9 "$(cat "$T/pid")" 2>/dev/null; rm -f "$T/pid"; }

# production build: the overrides are compiled out
if [ -x ./schema-ctl ]; then
    grep -qa SCHEMA_VERIFY_BIN ./schema-ctl && fail "production schema-ctl honours SCHEMA_VERIFY_BIN"
    grep -qa SCHEMA_VERIFY_CTL_SOCK ./schema-ctl && fail "production schema-ctl honours SCHEMA_VERIFY_CTL_SOCK"
fi

# a build that ignores the socket override would send reboot to the real PID 1: probe with status first
"$CTL" status < /dev/null > "$T/out" 2>&1
connected || fail "$CTL ignores SCHEMA_VERIFY_CTL_SOCK — not running reboot tests against the real socket"

# non-interactive reboot/poweroff: never gated, verify not even run
notty v1 reboot
connected || fail "non-tty reboot with a FAIL did not proceed"
[ -e "$T/ran" ] && fail "non-tty reboot ran the verifier"
notty v1 poweroff
connected || fail "non-tty poweroff with a FAIL did not proceed"

# reexec is gated with or without a terminal
notty v1 reexec
[ "$rc" = 1 ] || fail "reexec with a FAIL: exit $rc, want 1"
connected && fail "reexec with a FAIL contacted PID 1"
has "[FAIL] next boot: init=/sbin/nope missing" || fail "reexec refusal did not show the report"
failed_msg reexec || fail "reexec refusal message"
notty v2 reexec
connected && fail "reexec with an unaccepted CHANGE contacted PID 1"
refused_msg reexec || fail "reexec CHANGE refusal message"
notty v3 reexec
[ "$rc" = 1 ] || fail "reexec with UNRESOLVED: exit $rc, want 1"
connected && fail "reexec with UNRESOLVED contacted PID 1"
has "[UNRESOLVED] Next boot: needs root" || fail "reexec UNRESOLVED refusal did not show the report"
has "schema-ctl: reexec refused — verification could not resolve the above; run 'schema-ctl reexec --force' to override" || fail "reexec UNRESOLVED message"
notty v0 reexec
connected || fail "reexec with a clean verify did not proceed"
notty v1 reexec /usr/sbin/schema-init
connected && fail "reexec <path> with a FAIL contacted PID 1"
failed_msg "reexec /usr/sbin/schema-init" || fail "reexec <path> refusal message"
notty v1 reexec --force /usr/sbin/schema-init
connected || fail "reexec --force <path> did not proceed"
[ -e "$T/ran" ] && fail "reexec --force ran the verifier"
has "reexec --force" && fail "--force was passed on to PID 1"
notty v1 reexec /usr/sbin/schema-init --force
connected || fail "reexec <path> --force did not proceed"

# verifier unavailable: reexec refuses
notty vsegv reexec
connected && fail "reexec with a crashed verifier contacted PID 1"
has "schema-ctl: reexec refused — verification unavailable (" || fail "reexec crash message"
has "signal" || fail "reexec crash message does not say it was a signal"
notty v64 reexec
connected && fail "reexec with verifier exit 64 contacted PID 1"
has "schema-ctl: reexec refused — verification unavailable (" || fail "reexec exit-64 message"
has "usage: schema-ctl verify" && fail "the verifier's stderr reached the terminal, not the capture"
SCHEMA_VERIFY_BIN=$T/missing "$CTL" reexec < /dev/null > "$T/out" 2>&1
connected && fail "reexec with a missing verifier contacted PID 1"
has "schema-ctl: reexec refused — verification unavailable (" || fail "reexec missing-verifier message"
has "; run 'schema-ctl reexec --force' to override" || fail "reexec unavailable message does not name the override command"

# the verifier's stderr is captured with its report, never written to the caller's tty
notty v1err reexec
has "verify-said-on-stderr" || fail "the verifier's stderr is not in the refusal report"

# an inherited SIGCHLD=SIG_IGN would have the kernel reap the verifier and its verdict read as a pass
if command -v python3 >/dev/null 2>&1; then
    IGN='import os,signal,sys; signal.signal(signal.SIGCHLD, signal.SIG_IGN); os.execv(sys.argv[1], sys.argv[1:])'
    for k in 1 2 3; do
        rm -f "$T/ran"
        SCHEMA_VERIFY_BIN=$T/v1 python3 -c "$IGN" "$CTL" reexec < /dev/null > "$T/out" 2>&1
        connected && fail "reexec under SIGCHLD=SIG_IGN with a FAIL contacted PID 1 (run $k)"
        failed_msg reexec || fail "reexec under SIGCHLD=SIG_IGN: refusal message (run $k)"
    done
    SCHEMA_VERIFY_BIN=$T/v0 python3 -c "$IGN" "$CTL" reexec < /dev/null > "$T/out" 2>&1
    connected || fail "reexec under SIGCHLD=SIG_IGN with a clean verify did not proceed"
fi

# a verifier that won't die after the timeout's SIGKILL (D state): give up on it, don't hang
t0=$(date +%s)
SCHEMA_VERIFY_GATE_TIMEOUT_MS=1000 SCHEMA_VERIFY_GATE_NOKILL=1 SCHEMA_VERIFY_BIN=$T/vpid "$CTL" reexec < /dev/null > "$T/out" 2>&1
t=$(($(date +%s) - t0))
killpid
[ "$t" -le 6 ] || fail "unkillable verifier: gate took ${t}s, want ~3"
connected && fail "reexec with an unkillable verifier contacted PID 1"
has "schema-ctl: reexec refused — verification unavailable (timed out" || fail "unkillable verifier message"

# tiny deadlines end promptly (smoke only: the old double-now_ms race is too narrow to hit here)
t0=$(date +%s)
for k in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    SCHEMA_VERIFY_GATE_TIMEOUT_MS=1 SCHEMA_VERIFY_BIN=$T/vhang "$CTL" reexec < /dev/null > "$T/out" 2>&1
done
t=$(($(date +%s) - t0))
[ "$t" -le 10 ] || fail "1 ms timeout x20: gate took ${t}s, want ~0"

# SIGTERM while waiting kills the verifier with the caller
rm -f "$T/pid"
SCHEMA_VERIFY_BIN=$T/vpid "$CTL" reexec < /dev/null > "$T/out" 2>&1 &
cp=$!
i=0; while [ ! -s "$T/pid" ] && [ $i -lt 50 ]; do i=$((i + 1)); sleep 0.1; done
[ -s "$T/pid" ] || fail "verifier never started"
kill -TERM $cp
wait $cp
rc=$?
vp=$(cat "$T/pid")
sleep 0.3
if kill -0 "$vp" 2>/dev/null; then killpid; fail "SIGTERM to schema-ctl left the verifier running"; fi
[ "$rc" = 143 ] || fail "SIGTERM during the gate: exit $rc, want 143"
connected && fail "SIGTERM during the gate still contacted PID 1"

# no override: the gate runs this binary's own verify --quiet
env -u SCHEMA_VERIFY_BIN "$CTL" reexec < /dev/null > "$T/out" 2>&1
has "Result: " || fail "reexec without SCHEMA_VERIFY_BIN did not run schema-ctl verify --quiet"

if [ -n "$PTY" ]; then
    tty v1 reboot
    [ "$rc" = 1 ] || fail "interactive reboot with a FAIL: exit $rc, want 1"
    connected && fail "interactive reboot with a FAIL contacted PID 1"
    has "[FAIL] next boot: init=/sbin/nope missing" || fail "interactive refusal did not show the report"
    failed_msg reboot || fail "interactive reboot refusal message"
    tty v2 poweroff
    connected && fail "interactive poweroff with an unaccepted CHANGE contacted PID 1"
    refused_msg poweroff || fail "interactive poweroff refusal message"
    tty v0 reboot
    connected || fail "interactive reboot with a clean verify did not proceed"
    [ -e "$T/ran" ] || fail "interactive reboot did not run the verifier"
    tty v1 reboot --force
    connected || fail "interactive reboot --force did not proceed"
    [ -e "$T/ran" ] && fail "reboot --force ran the verifier"
    has "reboot --force" && fail "--force was passed on to PID 1"
    tty v1 poweroff --force
    connected || fail "interactive poweroff --force did not proceed"

    tty v3 reboot
    has "[WARN] Next boot: needs root" || fail "interactive reboot UNRESOLVED not shown as [WARN]"
    has "[UNRESOLVED]" && fail "interactive reboot UNRESOLVED still tagged UNRESOLVED"
    connected || fail "interactive reboot with UNRESOLVED did not proceed"
    awk '/\[WARN\] Next boot/ { w = NR } /No such file/ { c = NR } END { exit !(w && c > w) }' "$T/out" ||
        fail "interactive reboot UNRESOLVED: WARN not before the connect"

    tty vsegv poweroff
    has "[WARN] verification unavailable; proceeding with poweroff" || fail "interactive poweroff crash WARN"
    connected || fail "interactive poweroff with a crashed verifier did not proceed"
    tty v64 reboot
    has "[WARN] verification unavailable; proceeding with reboot" || fail "interactive reboot exit-64 WARN"
    connected || fail "interactive reboot with verifier exit 64 did not proceed"
    tty missing reboot
    has "[WARN] verification unavailable; proceeding with reboot" || fail "interactive reboot missing-verifier WARN"
    connected || fail "interactive reboot with a missing verifier did not proceed"

    # the shipped 105 s would make this the slowest test by far: 3 s stands in for it
    t0=$(date +%s)
    SCHEMA_VERIFY_GATE_TIMEOUT_MS=3000 tty vhang reboot
    t=$(($(date +%s) - t0))
    [ "$t" -ge 3 ] && [ "$t" -lt 8 ] || fail "hung verifier: gate took ${t}s, want ~3"
    has "[WARN] verification unavailable; proceeding with reboot" || fail "interactive reboot hang WARN"
    has "timed out" || fail "hang reason does not say timed out"
    connected || fail "interactive reboot with a hung verifier did not proceed"
    pgrep -f "$T/vhang" > /dev/null && fail "hung verifier left running"
fi

echo "verify_gate: ok"
