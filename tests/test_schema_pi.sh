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

KIT="pi-early udev udev-trigger dbus pi-timesync journal-sink pi-boot-firmware pi-bootok pi-deadline"
SVC=$R/etc/schema-init/services
REQ=$R/etc/schema-init/pi-boot-required

# importer shim: a test must never reach the real schema-import against /.
# Like the real one it never overwrites an existing .svc.
mkdir -p "$T/shim"
printf '%s\n' '#!/bin/sh' \
    'echo "$MIGRATE_ROOT|$SCHEMA_SVC_DIR|$*" >> "$SHIM_T/import.log"' \
    'i=0; e=0' \
    'for n in $(cat "$SHIM_T/import.units"); do' \
    '    if [ -e "$SCHEMA_SVC_DIR/$n.svc" ]; then' \
    '        echo "have    $n.service ($SCHEMA_SVC_DIR/$n.svc exists; --force to overwrite)"; e=$((e + 1))' \
    '    else' \
    '        echo "name=$n" > "$SCHEMA_SVC_DIR/$n.svc"' \
    '        echo "import  $n.service -> $SCHEMA_SVC_DIR/$n.svc"; i=$((i + 1))' \
    '    fi' \
    'done' \
    'cat "$SHIM_T/import.extra"' \
    'echo "imported=$i exists=$e not-found=0 skipped=$(grep -c "^skip" "$SHIM_T/import.extra") error=0"' \
    > "$T/shim/schema-import"
chmod +x "$T/shim/schema-import"

# wants links dangle in a fake root: they point at /lib, not $R/lib
wants() {
    rm -rf "$R/etc/systemd/system"
    mkdir -p "$R/etc/systemd/system/multi-user.target.wants"
    for u in "$@"; do
        ln -s "/lib/systemd/system/$u.service" "$R/etc/systemd/system/multi-user.target.wants/$u.service"
    done
}

reset() {
    rm -rf "$R" "$T/bin" "$T/import.log"
    mkdir -p "$R/boot/firmware" "$R/lib/systemd" "$R/usr/bin" "$R/usr/libexec/schema-init" \
        "$R/usr/share/schema-init" "$R/etc/schema-init/services" "$R/var/lib"
    printf '%s\n' "$STOCK" > "$CL"
    printf '#!/bin/sh\n' > "$R/lib/systemd/systemd"; chmod +x "$R/lib/systemd/systemd"
    echo bin-init > "$R/usr/bin/schema-init"
    echo bin-ctl > "$R/usr/bin/schema-ctl"
    echo b > "$R/usr/libexec/schema-init/schema-pi-bootok"
    echo d > "$R/usr/libexec/schema-init/schema-pi-deadline"
    for k in $KIT; do echo "$k-svc" > "$R/usr/share/schema-init/$k.svc.example"; done
    wants ssh NetworkManager
    echo "ssh NetworkManager" > "$T/import.units"
    echo "skip    foo.service: Type=forking without PIDFile= (see $SVC/foo.svc.skipped)" > "$T/import.extra"
}

# the boot-OK list minus comments and blanks
required() { grep -v '^[[:blank:]]*#' "$REQ" | grep -v '^[[:blank:]]*$'; }

pi() {
    SCHEMA_PI_ROOT=$R SCHEMA_PI_BOOTOK=$PWD/scripts/schema-pi-bootok \
        SCHEMA_IMPORT=$T/shim/schema-import SHIM_T=$T \
        SCHEMA_PROC1=$T/comm ${TEST_SH:-sh} "$PI" "$@"
}

echo systemd > "$T/comm"

# enable_fresh
reset
pi enable > "$T/log" 2>&1 || fail "enable_fresh: exit: $(cat "$T/log")"
[ "$(cat "$CL")" = "$STOCK panic=10 init=/usr/sbin/schema-init-pi" ] || fail "enable_fresh: line"
[ "$(wc -l < "$CL")" -eq 1 ] || fail "enable_fresh: newline count"
printf '%s\n' "$STOCK" | cmp -s - "$BAK" || fail "enable_fresh: backup"
for k in $KIT; do
    [ "$(cat "$SVC/$k.svc")" = "$k-svc" ] || fail "enable_fresh: kit $k.svc"
done
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
    [ ! -e "$T/import.log" ] || fail "$name: importer ran"
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

# enable_imports: kit first, then the importer, then the network drop-in
reset
pi enable > "$T/log" 2>&1 || fail "enable_imports: exit: $(cat "$T/log")"
[ "$(cat "$T/import.log")" = "$R|$SVC|--enabled" ] || fail "enable_imports: call: $(cat "$T/import.log")"
[ "$(cat "$SVC/ssh.svc")" = name=ssh ] || fail "enable_imports: ssh.svc"
printf 'dep=udev-trigger\ndep=dbus\n' | cmp -s - "$SVC/NetworkManager.svc.d/pi-kit.conf" ||
    fail "enable_imports: drop-in: $(cat "$SVC/NetworkManager.svc.d/pi-kit.conf" 2>&1)"
[ "$(sed -n 1p "$T/log")" = "schema-pi: imported 2, skipped 1" ] || fail "enable_imports: summary: $(cat "$T/log")"
[ "$(sed -n 2p "$T/log")" = "skip foo.service: Type=forking without PIDFile=" ] || fail "enable_imports: skip line: $(cat "$T/log")"
echo "ok enable_imports"

# boot_required: the kit core, ssh and the network, nothing else
[ "$(required | tr '\n' ' ')" = "udev udev-trigger dbus pi-early ssh NetworkManager " ] ||
    fail "boot_required: $(cat "$REQ")"
echo "ok boot_required"

# boot_required_pihole: unbound counts only beside pihole-FTL
reset; echo "ssh NetworkManager pihole-FTL unbound" > "$T/import.units"
pi enable > "$T/log" 2>&1 || fail "boot_required_pihole: exit: $(cat "$T/log")"
[ "$(required | tr '\n' ' ')" = "udev udev-trigger dbus pi-early ssh NetworkManager pihole-FTL unbound " ] ||
    fail "boot_required_pihole: $(cat "$REQ")"
reset; echo "ssh NetworkManager pihole-FTL" > "$T/import.units"
pi enable > "$T/log" 2>&1 || fail "boot_required_pihole: alone exit"
[ "$(required | tr '\n' ' ')" = "udev udev-trigger dbus pi-early ssh NetworkManager pihole-FTL " ] ||
    fail "boot_required_pihole: alone: $(cat "$REQ")"
reset; echo "ssh NetworkManager unbound" > "$T/import.units"
pi enable > "$T/log" 2>&1 || fail "boot_required_pihole: unbound exit"
! required | grep -qx unbound || fail "boot_required_pihole: unbound without pihole"
echo "ok boot_required_pihole"

# network_order: first enabled in the fixed order wins
reset; wants ssh wpa_supplicant; echo "ssh wpa_supplicant" > "$T/import.units"
pi enable > "$T/log" 2>&1 || fail "network_wifi: exit: $(cat "$T/log")"
[ "$(required | tail -n 1)" = wpa_supplicant ] || fail "network_wifi: $(cat "$REQ")"
[ -f "$SVC/wpa_supplicant.svc.d/pi-kit.conf" ] || fail "network_wifi: drop-in"
reset; wants ssh dhcpcd systemd-networkd wpa_supplicant
echo "ssh dhcpcd systemd-networkd wpa_supplicant" > "$T/import.units"
pi enable > "$T/log" 2>&1 || fail "network_order: exit: $(cat "$T/log")"
[ "$(required | tail -n 1)" = systemd-networkd ] || fail "network_order: $(cat "$REQ")"
[ ! -e "$SVC/dhcpcd.svc.d" ] || fail "network_order: drop-in on the loser"
echo "ok network_order"

# sshd_name: the importer may name it sshd
reset; wants sshd NetworkManager; echo "sshd NetworkManager" > "$T/import.units"
pi enable > "$T/log" 2>&1 || fail "sshd_name: exit: $(cat "$T/log")"
required | grep -qx sshd || fail "sshd_name: $(cat "$REQ")"
echo "ok sshd_name"

# no_ssh_after_import: replaces the old pre-import sshd.svc precondition
reset; echo "NetworkManager" > "$T/import.units"
pi enable > "$T/log" 2>&1; rc=$?
[ $rc -eq 1 ] || fail "no_ssh: exit $rc"
grep -qx 'schema-pi: no ssh service after import: schema-init would boot without ssh' "$T/log" ||
    fail "no_ssh: message: $(cat "$T/log")"
printf '%s\n' "$STOCK" | cmp -s - "$CL" || fail "no_ssh: cmdline changed"
[ ! -e "$BAK" ] || fail "no_ssh: backup written"
[ ! -e "$REQ" ] || fail "no_ssh: list written"
echo "ok no_ssh_after_import"

# no_network: ssh running on an unreachable box must not pass as a good boot
reset; wants ssh; echo "ssh" > "$T/import.units"
pi enable > "$T/log" 2>&1; rc=$?
[ $rc -eq 1 ] || fail "no_network: exit $rc"
grep -q 'no enabled network service' "$T/log" || fail "no_network: message: $(cat "$T/log")"
printf '%s\n' "$STOCK" | cmp -s - "$CL" || fail "no_network: cmdline changed"
reset; echo "ssh" > "$T/import.units"
pi enable > "$T/log" 2>&1; rc=$?
[ $rc -eq 1 ] || fail "no_network_svc: exit $rc"
grep -q 'NetworkManager.svc missing after import' "$T/log" || fail "no_network_svc: message: $(cat "$T/log")"
printf '%s\n' "$STOCK" | cmp -s - "$CL" || fail "no_network_svc: cmdline changed"
echo "ok no_network"

# import_fails: no summary line means the importer died
reset; printf '#!/bin/sh\necho boom >&2\nexit 1\n' > "$T/badimport"; chmod +x "$T/badimport"
SCHEMA_IMPORT=$T/badimport SCHEMA_PI_ROOT=$R SCHEMA_PROC1=$T/comm sh "$PI" enable > "$T/log" 2>&1; rc=$?
[ $rc -eq 1 ] || fail "import_fails: exit $rc"
grep -q 'schema-import failed' "$T/log" || fail "import_fails: message: $(cat "$T/log")"
printf '%s\n' "$STOCK" | cmp -s - "$CL" || fail "import_fails: cmdline changed"
echo "ok import_fails"

# user_files_win: an existing .svc keeps its bytes
reset; echo my-ssh > "$SVC/ssh.svc"; echo my-dbus > "$SVC/dbus.svc"
pi enable > "$T/log" 2>&1 || fail "user_files_win: exit: $(cat "$T/log")"
[ "$(cat "$SVC/ssh.svc")" = my-ssh ] || fail "user_files_win: ssh.svc"
[ "$(cat "$SVC/dbus.svc")" = my-dbus ] || fail "user_files_win: dbus.svc"
echo "ok user_files_win"

# rerun: list rewritten identically, drop-in not duplicated
reset; pi enable > "$T/log" 2>&1 || fail "rerun: first"
cp "$REQ" "$T/req1"; cp "$SVC/NetworkManager.svc.d/pi-kit.conf" "$T/drop1"
pi enable > "$T/log" 2>&1 || fail "rerun: second: $(cat "$T/log")"
cmp -s "$REQ" "$T/req1" || fail "rerun: list differs"
cmp -s "$SVC/NetworkManager.svc.d/pi-kit.conf" "$T/drop1" || fail "rerun: drop-in differs"
[ "$(ls "$SVC/NetworkManager.svc.d")" = pi-kit.conf ] || fail "rerun: extra drop-ins"
echo "ok rerun"

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
