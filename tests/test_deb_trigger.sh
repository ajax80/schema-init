#!/bin/sh
set -u
cd "$(dirname "$0")/.."
POSTINST=$PWD/debian/schema-init.postinst
PRERM=$PWD/debian/schema-init.prerm
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
# prerm: removing the package must not leave cmdline.txt pointing at a deleted init
prerm() {
    rm -f "$T/pi.log"
    PATH=$T/bin:$PATH SCHEMA_PI_MOUNT_FIRMWARE=$T/bin/mount-fw sh "$PRERM" "$@" > "$T/out" 2>&1
}
printf '#!/bin/sh\necho mount >> "%s/pi.log"\n' "$T" > "$T/bin/mount-fw"
chmod +x "$T/bin/mount-fw"
pi_shim() {
    printf '#!/bin/sh\necho "$*" >> "%s/pi.log"\n[ "$1" = status ] && echo "enabled: %s"\n[ "$1" = disable ] && exit %s\nexit 0\n' "$T" "$1" "$2" > "$T/bin/schema-pi"
    chmod +x "$T/bin/schema-pi"
}
pi_shim yes 0
prerm remove || fail "prerm remove exit"
grep -qx disable "$T/pi.log" || fail "prerm remove did not disable: $(cat "$T/pi.log" 2>/dev/null)"
[ "$(head -n 1 "$T/pi.log")" = mount ] || fail "prerm must mount /boot/firmware before status: $(cat "$T/pi.log")"
prerm upgrade 0.4.20-1 || fail "prerm upgrade exit"
! grep -q disable "$T/pi.log" 2>/dev/null || fail "prerm upgrade disabled"
pi_shim no 0
prerm remove || fail "prerm remove/not enabled exit"
! grep -q disable "$T/pi.log" || fail "prerm disabled when not enabled"
pi_shim yes 1
prerm remove || fail "prerm remove must not fail when disable does"
# an empty PATH, not just no shim: the host's real schema-pi must never run here
mkdir "$T/empty"
PATH=$T/empty /bin/sh "$PRERM" remove > "$T/out" 2>&1 || fail "prerm remove without schema-pi"
echo ok prerm

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
rm -f "$T/bin/schema-pi"
run configure 0.4.19-1 || fail "upgrade configure exit"
! grep -q "schema-pi enable" "$T/out" || fail "enable hint on upgrade"

# upgrade refreshes the Pi kit; a fresh install doesn't, and a failed refresh never fails configure
pi_shim yes 0
rm -f "$T/pi.log"
run configure || fail "configure exit with schema-pi"
[ ! -e "$T/pi.log" ] || fail "fresh install ran schema-pi: $(cat "$T/pi.log")"
run configure 0.4.19-1 || fail "upgrade configure exit with schema-pi"
grep -qx refresh "$T/pi.log" || fail "upgrade did not refresh: $(cat "$T/pi.log" 2>&1)"
printf '#!/bin/sh\nexit 1\n' > "$T/bin/schema-pi"
run configure 0.4.19-1 || fail "failed refresh failed configure"
rm -f "$T/bin/schema-pi"

echo ok test_deb_trigger
