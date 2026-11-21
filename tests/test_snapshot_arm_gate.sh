#!/bin/sh
# schema-snapshot arm runs verify first and arms nothing unless it passes; --force skips it.
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
rc=0
fail() { echo "FAIL: $*"; rc=1; }
mkdir "$T/bin"
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$T/bin/$1"; chmod 755 "$T/bin/$1"; }
stub id 'echo 0'
stub findmnt 'case "$*" in *FSTYPE*) echo btrfs ;; *SOURCE*) echo /dev/fake ;; *) exit 1 ;; esac'
stub mount 'exit 1'
stub umount 'exit 1'
stub grub2-editenv 'echo "$*" >> '"$T"'/editenv'
stub schema-ctl 'echo "$*" >> '"$T"'/ctl; echo "[FAIL] next boot: stub"; exit ${VRC:-0}'

arm() { rm -f "$T/editenv" "$T/ctl"; PATH=$T/bin:$PATH bash scripts/schema-snapshot arm "$@" > "$T/out" 2>&1; }

for v in 1 2 3 64; do
    VRC=$v arm; r=$?
    [ $r != 0 ] || fail "verify $v: arm exited 0"
    [ ! -e "$T/editenv" ] || fail "verify $v: grubenv was written"
    grep -q '^verify --quiet --arming$' "$T/ctl" 2>/dev/null || fail "verify $v: verify not run"
    grep -q 'not armed' "$T/out" || fail "verify $v: no refusal: $(cat "$T/out")"
    grep -q '\[FAIL\] next boot: stub' "$T/out" || fail "verify $v: report not shown"
done
VRC=1 arm; grep -q 'verify --accept' "$T/out" && fail "verify 1: offers --accept for a FAIL"
VRC=2 arm; grep -q 'verify --accept' "$T/out" || fail "verify 2: no --accept hint"
VRC=3 arm; grep -q 'could not resolve' "$T/out" || fail "verify 3: wording"
VRC=64 arm; grep -q 'unavailable (schema-ctl verify exited 64)' "$T/out" || fail "verify 64: wording"

# verify passes / --force: the gate lets it through to check_armable (which this
# host's real /boot decides; only that the gate didn't stop it matters here)
VRC=0 arm 3
grep -q 'not armed' "$T/out" && fail "verify 0: refused"
grep -q '^verify --quiet --arming$' "$T/ctl" 2>/dev/null || fail "verify 0: verify not run"
VRC=1 arm --force 3
grep -q 'not armed' "$T/out" && fail "--force: refused"
[ ! -e "$T/ctl" ] || fail "--force: verify ran: $(cat "$T/ctl")"

# anything but --force and a count is refused before verify runs or grubenv is touched
for bad in -f --forse 2x ""; do
    VRC=0 arm "$bad"; r=$?
    [ $r != 0 ] || fail "arm '$bad': exited 0"
    [ ! -e "$T/editenv" ] || fail "arm '$bad': grubenv written"
    [ ! -e "$T/ctl" ] || fail "arm '$bad': verify ran"
    grep -q 'usage' "$T/out" || fail "arm '$bad': no usage: $(cat "$T/out")"
done

[ $rc = 0 ] && echo "snapshot_arm_gate: ok"
exit $rc
