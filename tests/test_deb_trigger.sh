#!/bin/sh
set -u
cd "$(dirname "$0")/.."
POSTINST=$PWD/debian/schema-init.postinst
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

mkdir "$T/bin"
printf '#!/bin/sh\necho "$@" >> "%s/ctl.log"\n' "$T" > "$T/bin/schema-ctl"
printf '#!/bin/sh\necho "$@" >> "%s/dpkg-trigger.log"\n' "$T" > "$T/bin/dpkg-trigger"
chmod +x "$T/bin/schema-ctl" "$T/bin/dpkg-trigger"
: > "$T/sock"

run() {
    rm -f "$T/ctl.log" "$T/dpkg-trigger.log"
    PATH=$T/bin:$PATH SCHEMA_PROC1=$T/comm SCHEMA_PROC1_ROOT=/ SCHEMA_CTL_SOCK=${SOCK:-$T/sock} \
        sh "$POSTINST" "$@" > "$T/out" 2>&1
}
# The postinst checks -S, which needs a real unix socket.
if command -v python3 >/dev/null 2>&1; then
    rm -f "$T/sock"
    python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); import time; time.sleep(30)' "$T/sock" &
    i=0; while [ ! -S "$T/sock" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
    trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
fi
[ -S "$T/sock" ] || { echo "skipped (no python3 for a unix socket)"; exit 0; }

echo systemd > "$T/comm"
run triggered schema-init-reexec || fail "triggered/systemd exit"
[ ! -e "$T/ctl.log" ] || fail "reexec on systemd PID 1"

echo schema-init > "$T/comm"
run triggered schema-init-reexec || fail "triggered/schema-init exit"
[ "$(cat "$T/ctl.log")" = reexec ] || fail "expected one reexec, got: $(cat "$T/ctl.log" 2>/dev/null)"

SOCK=$T/nosock run triggered schema-init-reexec || fail "no-socket exit"
[ ! -e "$T/ctl.log" ] || fail "reexec without socket"

printf '#!/bin/sh\nexit 1\n' > "$T/bin/schema-ctl"
run triggered schema-init-reexec || fail "failed reexec must exit 0"
grep -q "kept its old binary" "$T/out" || fail "missing fallback message"
printf '#!/bin/sh\necho "$@" >> "%s/ctl.log"\n' "$T" > "$T/bin/schema-ctl"

run configure || fail "configure exit"
grep -q "schema-init-reexec" "$T/dpkg-trigger.log" || fail "configure did not call dpkg-trigger"
grep -q -- --no-await "$T/dpkg-trigger.log" || fail "dpkg-trigger without --no-await"
[ ! -e "$T/ctl.log" ] || fail "configure called schema-ctl"
grep -q "schema-pi enable" "$T/out" || fail "no enable hint on first install"
run configure 0.4.19-1 || fail "upgrade configure exit"
! grep -q "schema-pi enable" "$T/out" || fail "enable hint on upgrade"

echo ok test_deb_trigger
