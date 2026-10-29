#!/bin/sh
set -u
cd "$(dirname "$0")/.."
PI=$PWD/scripts/schema-pi
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

R=$T/root
CL=$R/boot/firmware/cmdline.txt
BAK=$CL.schema-bak
STOCK='console=serial0,115200 console=tty1 root=PARTUUID=abcd-02 rootfstype=ext4 fsck.repair=yes rootwait quiet'

reset() {
    rm -rf "$R" "$T/bin"
    mkdir -p "$R/boot/firmware" "$R/lib/systemd" "$R/usr/bin" "$R/usr/libexec/schema-init" \
        "$R/usr/share/schema-init" "$R/etc/schema-init/services" "$R/var/lib"
    printf '%s\n' "$STOCK" > "$CL"
    printf '#!/bin/sh\n' > "$R/lib/systemd/systemd"; chmod +x "$R/lib/systemd/systemd"
    echo bin-init > "$R/usr/bin/schema-init"
    echo bin-ctl > "$R/usr/bin/schema-ctl"
    echo svc > "$R/etc/schema-init/services/sshd.svc"
    echo b > "$R/usr/libexec/schema-init/schema-pi-bootok"
    echo d > "$R/usr/libexec/schema-init/schema-pi-deadline"
    echo bootok-svc > "$R/usr/share/schema-init/pi-bootok.svc.example"
    echo deadline-svc > "$R/usr/share/schema-init/pi-deadline.svc.example"
    echo firmware-svc > "$R/usr/share/schema-init/pi-boot-firmware.svc.example"
}

pi() {
    SCHEMA_PI_ROOT=$R SCHEMA_PI_BOOTOK=$PWD/scripts/schema-pi-bootok \
        SCHEMA_PROC1=$T/comm ${TEST_SH:-sh} "$PI" "$@"
}

echo systemd > "$T/comm"

# enable_fresh
reset
pi enable > "$T/log" 2>&1 || fail "enable_fresh: exit: $(cat "$T/log")"
[ "$(cat "$CL")" = "$STOCK panic=10 init=/usr/sbin/schema-init-pi" ] || fail "enable_fresh: line"
[ "$(wc -l < "$CL")" -eq 1 ] || fail "enable_fresh: newline count"
printf '%s\n' "$STOCK" | cmp -s - "$BAK" || fail "enable_fresh: backup"
[ "$(cat "$R/etc/schema-init/services/pi-bootok.svc")" = bootok-svc ] || fail "enable_fresh: bootok svc"
[ "$(cat "$R/etc/schema-init/services/pi-deadline.svc")" = deadline-svc ] || fail "enable_fresh: deadline svc"
[ "$(cat "$R/etc/schema-init/services/pi-boot-firmware.svc")" = firmware-svc ] || fail "enable_fresh: firmware svc"
[ "$(cat "$R/var/lib/schema-init/pi-boot")" = "attempts=0
last_target=current" ] || fail "enable_fresh: state"
cmp -s "$R/var/lib/schema-init/known-good/schema-init" "$R/usr/bin/schema-init" || fail "enable_fresh: known-good"
echo "ok enable_fresh"

# enable_twice
cp "$CL" "$T/once"
echo mine > "$R/etc/schema-init/services/pi-bootok.svc"
pi enable > "$T/log" 2>&1 || fail "enable_twice: exit"
cmp -s "$CL" "$T/once" || fail "enable_twice: bytes differ"
printf '%s\n' "$STOCK" | cmp -s - "$BAK" || fail "enable_twice: backup rewritten"
[ "$(cat "$R/etc/schema-init/services/pi-bootok.svc")" = mine ] || fail "enable_twice: svc overwritten"
echo "ok enable_twice"

# enable_twice_keeps_kg
echo changed > "$R/usr/bin/schema-init"
printf 'attempts=2\nlast_target=current\n' > "$R/var/lib/schema-init/pi-boot"
pi enable > "$T/log" 2>&1 || fail "enable_twice_keeps_kg: exit"
[ "$(cat "$R/var/lib/schema-init/known-good/schema-init")" = bin-init ] || fail "enable_twice_keeps_kg: kg overwritten"
[ "$(cat "$R/var/lib/schema-init/pi-boot")" = "attempts=2
last_target=current" ] || fail "enable_twice_keeps_kg: state reset"
echo "ok enable_twice_keeps_kg"

# enable_replaces_old_init
reset
printf 'console=tty1 init=/sbin/schema-init-log rootwait init=/x quiet\n' > "$CL"
pi enable > "$T/log" 2>&1 || fail "enable_replaces_old_init: exit"
[ "$(cat "$CL")" = "console=tty1 rootwait quiet panic=10 init=/usr/sbin/schema-init-pi" ] || fail "enable_replaces_old_init: $(cat "$CL")"
echo "ok enable_replaces_old_init"

# enable_crlf (and extra lines dropped)
reset
printf '%s\r\nsecond line\r\n' "$STOCK" > "$CL"
pi enable > "$T/log" 2>&1 || fail "enable_crlf: exit"
[ "$(cat "$CL")" = "$STOCK panic=10 init=/usr/sbin/schema-init-pi" ] || fail "enable_crlf: line"
[ "$(wc -l < "$CL")" -eq 1 ] || fail "enable_crlf: line count"
[ -z "$(tr -d '\r' < "$CL" | cmp - "$CL" 2>&1)" ] || fail "enable_crlf: CR remains"
echo "ok enable_crlf"

# enable_adds_panic: an early PID 1 crash must reboot, not hang, so the counter advances
reset
pi enable > "$T/log" 2>&1 || fail "enable_adds_panic: exit"
[ "$(grep -o 'panic=[^[:blank:]]*' "$CL")" = panic=10 ] || fail "enable_adds_panic: $(cat "$CL")"
pi enable > "$T/log" 2>&1 || fail "enable_adds_panic: re-enable"
[ "$(grep -o 'panic=[^[:blank:]]*' "$CL")" = panic=10 ] || fail "enable_adds_panic: duplicated: $(cat "$CL")"
[ "$(wc -l < "$CL")" -eq 1 ] || fail "enable_adds_panic: line count"
reset
printf 'console=tty1 panic=5 rootwait\n' > "$CL"
pi enable > "$T/log" 2>&1 || fail "enable_adds_panic: existing"
[ "$(cat "$CL")" = "console=tty1 panic=5 rootwait init=/usr/sbin/schema-init-pi" ] || fail "enable_adds_panic: existing kept: $(cat "$CL")"
echo "ok enable_adds_panic"

refuses() {
    name=$1
    rm -f "$T/orig"; cp "$CL" "$T/orig" 2>/dev/null
    pi enable > "$T/log" 2>&1; rc=$?
    [ $rc -eq 1 ] || fail "$name: exit $rc"
    [ "$(wc -l < "$T/log")" -eq 1 ] || fail "$name: message: $(cat "$T/log")"
    [ ! -e "$BAK" ] || fail "$name: backup written"
    [ ! -e "$R/etc/schema-init/services/pi-bootok.svc" ] || fail "$name: svc installed"
    [ ! -e "$R/var/lib/schema-init" ] || fail "$name: state dir created"
    [ ! -e "$T/orig" ] || cmp -s "$CL" "$T/orig" || fail "$name: file modified"
    echo "ok $name"
}

# enable_refuses_no_systemd
reset; rm "$R/lib/systemd/systemd"; refuses enable_refuses_no_systemd

# enable_refuses_noexec
reset; mkdir "$T/bin"
printf '#!/bin/sh\necho rw,nosuid,noexec,relatime\n' > "$T/bin/findmnt"; chmod +x "$T/bin/findmnt"
PATH=$T/bin:$PATH refuses enable_refuses_noexec
# a mount option merely containing the letters must not refuse
printf '#!/bin/sh\necho rw,noexecfoo,relatime\n' > "$T/bin/findmnt"
PATH=$T/bin:$PATH pi enable > "$T/log" 2>&1 || fail "noexec_wholeword: $(cat "$T/log")"
echo "ok noexec_wholeword"

# enable_refuses_no_cmdline
reset; rm "$CL"; refuses enable_refuses_no_cmdline
[ ! -e "$CL" ] || fail "enable_refuses_no_cmdline: created"

# enable_refuses_no_sshd
reset; rm "$R/etc/schema-init/services/sshd.svc"; refuses enable_refuses_no_sshd

# disable_roundtrip
reset
pi enable > "$T/log" 2>&1 || fail "disable_roundtrip: enable"
pi disable > "$T/log" 2>&1 || fail "disable_roundtrip: disable"
# panic=10 is left in on purpose
printf '%s panic=10\n' "$STOCK" | cmp -s - "$CL" || fail "disable_roundtrip: $(cat "$CL")"
echo "ok disable_roundtrip"

# disable_keeps_whitespace
printf 'a  b\tc init=/x  d init=/y\n' > "$CL"
pi disable > "$T/log" 2>&1 || fail "disable_keeps_whitespace: exit"
printf 'a  b\tc  d\n' | cmp -s - "$CL" || fail "disable_keeps_whitespace: $(cat "$CL")"
printf 'init=/x quiet\n' > "$CL"
pi disable > "$T/log" 2>&1 || fail "disable_leading: exit"
printf 'quiet\n' | cmp -s - "$CL" || fail "disable_leading: $(cat "$CL")"
echo "ok disable_keeps_whitespace"

# status_prints
reset
pi status > "$T/log" 2>&1 || fail "status_prints: exit"
grep -qx 'enabled: no' "$T/log" && grep -qx 'pid1: systemd' "$T/log" &&
    grep -qx 'attempts: -' "$T/log" && grep -qx 'last_target: -' "$T/log" &&
    grep -qx 'last_fallback: -' "$T/log" && grep -qx 'known_good: -' "$T/log" || fail "status_prints: empty: $(cat "$T/log")"
pi enable > /dev/null 2>&1
printf '#!/bin/sh\necho schema-init 9.9\n' > "$R/var/lib/schema-init/known-good/schema-init"
chmod +x "$R/var/lib/schema-init/known-good/schema-init"
mkdir -p "$R/var/log/schema-init"
printf '1 fallback=known-good attempts=3\n2 fallback=systemd attempts=3\n' > "$R/var/log/schema-init/fallback.log"
echo schema-init-pi > "$T/comm"
pi status > "$T/log" 2>&1 || fail "status_prints: exit2"
grep -qx 'enabled: yes' "$T/log" && grep -qx 'pid1: schema-init-pi' "$T/log" &&
    grep -qx 'attempts: 0' "$T/log" && grep -qx 'last_target: current' "$T/log" &&
    grep -qx 'last_fallback: 2 fallback=systemd attempts=3' "$T/log" &&
    grep -qx 'known_good: schema-init 9.9' "$T/log" || fail "status_prints: full: $(cat "$T/log")"
echo "ok status_prints"

# firmware_mount: schema-init has no fstab automount, so /boot/firmware may not be mounted
# shims only: a real mount must never run here
fw_shims() {
    rm -rf "$T/bin"; mkdir "$T/bin"
    printf '#!/bin/sh\necho "$*" >> "%s/mount.log"\nexit %s\n' "$T" "$1" > "$T/bin/mount"
    printf '#!/bin/sh\necho "$*" >> "%s/mountpoint.log"\nexit %s\n' "$T" "$2" > "$T/bin/mountpoint"
    chmod +x "$T/bin/mount" "$T/bin/mountpoint"
    rm -f "$T/mount.log" "$T/mountpoint.log"
}
FSTAB='PARTUUID=abcd-01  /boot/firmware  vfat    defaults          0       2'
for cmd in enable disable; do
    reset; printf '%s\n' "$FSTAB" > "$R/etc/fstab"
    fw_shims 0 1
    PATH=$T/bin:$PATH pi $cmd > "$T/log" 2>&1 || fail "firmware_mount $cmd: exit: $(cat "$T/log")"
    [ "$(cat "$T/mount.log" 2>/dev/null)" = "$R/boot/firmware" ] || fail "firmware_mount $cmd: mount: $(cat "$T/mount.log" 2>/dev/null)"
    fw_shims 0 0
    PATH=$T/bin:$PATH pi $cmd > "$T/log" 2>&1 || fail "firmware_mount $cmd: mounted exit"
    [ ! -e "$T/mount.log" ] || fail "firmware_mount $cmd: remounted a mountpoint"
    # a failed mount must stop before cmdline.txt is touched
    cp "$CL" "$T/orig"
    fw_shims 32 1
    PATH=$T/bin:$PATH pi $cmd > "$T/log" 2>&1; rc=$?
    [ $rc -eq 1 ] || fail "firmware_mount $cmd: failed mount exit $rc"
    grep -q 'cannot mount' "$T/log" || fail "firmware_mount $cmd: message: $(cat "$T/log")"
    cmp -s "$CL" "$T/orig" || fail "firmware_mount $cmd: cmdline touched"
done
reset; printf '# %s\n/dev/sda1 /boot ext4 defaults 0 2\n' "$FSTAB" > "$R/etc/fstab"
fw_shims 0 1
PATH=$T/bin:$PATH pi enable > "$T/log" 2>&1 || fail "firmware_mount unlisted: exit"
[ ! -e "$T/mount.log" ] && [ ! -e "$T/mountpoint.log" ] || fail "firmware_mount unlisted: mount called"
echo "ok firmware_mount"

# firmware_helper: the boot oneshot mounts it the same way and never fails the boot
FW=$PWD/scripts/schema-pi-mount-firmware
reset; printf '%s\n' "$FSTAB" > "$R/etc/fstab"
fw_shims 32 1
PATH=$T/bin:$PATH SCHEMA_PI_ROOT=$R sh "$FW" > "$T/log" 2>&1 || fail "firmware_helper: failed mount exit"
[ "$(cat "$T/mount.log")" = "$R/boot/firmware" ] || fail "firmware_helper: mount"
fw_shims 0 0
PATH=$T/bin:$PATH SCHEMA_PI_ROOT=$R sh "$FW" > "$T/log" 2>&1 || fail "firmware_helper: exit"
[ ! -e "$T/mount.log" ] || fail "firmware_helper: remounted"
rm "$R/etc/fstab"; fw_shims 0 1
PATH=$T/bin:$PATH SCHEMA_PI_ROOT=$R sh "$FW" > "$T/log" 2>&1 || fail "firmware_helper: no fstab exit"
[ ! -e "$T/mount.log" ] || fail "firmware_helper: mounted without fstab entry"
echo "ok firmware_helper"

# write_fails: a failed mv must not report success
reset; rm -rf "$T/bin"; mkdir "$T/bin"
printf '#!/bin/sh\nexit 1\n' > "$T/bin/mv"; chmod +x "$T/bin/mv"
PATH=$T/bin:$PATH pi disable > "$T/log" 2>&1; rc=$?
[ $rc -eq 1 ] || fail "write_fails: exit $rc"
grep -q 'cannot replace' "$T/log" || fail "write_fails: message: $(cat "$T/log")"
rm -rf "$T/bin" "$CL".tmp.*
echo "ok write_fails"

# usage
pi > /dev/null 2>&1; [ $? -eq 2 ] || fail "usage: none"
pi bogus > /dev/null 2>&1; [ $? -eq 2 ] || fail "usage: bogus"
echo "ok usage"
