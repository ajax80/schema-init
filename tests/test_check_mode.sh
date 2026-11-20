#!/bin/sh
# schema-init --check: a target root's services through PID 1's own loader.
# Fixtures are made here, not committed: git keeps no modes, and the loader
# refuses a group/world-writable .svc (one case below relies on exactly that).
set -u
cd "$(dirname "$0")/.."
[ -x ./schema-init ] || { echo "FAIL check_mode: no ./schema-init"; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
chmod 0755 "$T"
umask 022
fail() { echo "FAIL check_mode: $*"; [ -f "$T/out" ] && sed 's/^/  | /' "$T/out"; exit 1; }

newroot() { R=$T/$1; mkdir -p "$R/etc/schema-init/services"; S=$R/etc/schema-init/services; }
svc() { printf "name=$2\nexec=${3:-/bin/true}\n${4:-}" > "$1/$2.svc"; }
check() { ./schema-init --check "$@" > "$T/out" 2> "$T/err"; rc=$?; }
has() { grep -qF -- "$1" "$T/out"; }

# clean dir
newroot clean
svc "$S" a; svc "$S" b /bin/sleep 'dep=a\n'
check --root "$R" --cmdline "root=UUID=1 quiet init=/sbin/schema-init rd.lvm=0 modprobe.blacklist=x"
[ $rc -eq 0 ] || fail "clean: exit $rc"
[ "$(head -n1 "$T/out")" = "schema-verify-baseline v1" ] || fail "clean: line 1"
[ "$(grep -c '^a\.exec=' "$T/out")" = 1 ] && [ "$(grep -c '^b\.exec=' "$T/out")" = 1 ] || fail "clean: exec lines"
has "binary_version=" || fail "clean: binary_version"
has "svc_dir=/etc/schema-init/services" || fail "clean: svc_dir"
has "cmdline.root=UUID=1" && has "cmdline.init=/sbin/schema-init" && has "cmdline.rd.lvm=0" &&
    has "cmdline.modprobe.blacklist=x" || fail "clean: cmdline lines"
has "cmdline.quiet" && fail "clean: non-boot-critical cmdline word echoed"
grep -q '^error:' "$T/out" && fail "clean: error line"
has "next_init=" && fail "clean: next_init off the Pi"
# body after the header is bytewise sorted
sed -n '/^b\.\|^a\./,$p' "$T/out" | LC_ALL=C sort -c 2>/dev/null || fail "clean: body not sorted"

# parse error
newroot parse
svc "$S" a; printf 'name=bad\nexec=/bin/true\ndynamic_user=bogus\n' > "$S/bad.svc"
check --root "$R" --cmdline ""
[ $rc -eq 1 ] || fail "parse: exit $rc"
grep -q '^error: bad\.svc:' "$T/out" || fail "parse: no error line"
has "a.exec=" || fail "parse: good service lost"

# cycle
newroot cycle
svc "$S" a /bin/true 'dep=b\n'; svc "$S" b /bin/true 'dep=a\n'
check --root "$R" --cmdline ""
[ $rc -eq 1 ] || fail "cycle: exit $rc"
grep -q '^error:.*cycle' "$T/out" || fail "cycle: no error line"

# group-writable: refused by owner/mode, must be named, not "missing"
newroot gw
svc "$S" a; svc "$S" gw; chmod 0664 "$S/gw.svc"
check --root "$R" --cmdline ""
[ $rc -eq 1 ] || fail "gw: exit $rc"
grep -q '^error: gw\.svc:' "$T/out" || fail "gw: no error line"

# unresolved dep is satisfied for PID 1; a group dep resolves without noise
newroot deps
svc "$S" a /bin/true 'dep=nosuch\n'; svc "$S" b /bin/true 'dep=display-stack\n'; svc "$S" c
printf 'name=display-stack\nmember=c\n' > "$S/display-stack.grp"
check --root "$R" --cmdline ""
[ $rc -eq 0 ] || fail "deps: exit $rc"
grep -q '^note:.*display-stack' "$T/out" && fail "deps: note about group dep"
has "grp.display-stack.members=c" || fail "deps: group membership line"

# a default that would hide exec is dropped, and says so
newroot drop
svc "$S" x /home/u/bin/x
check --root "$R" --cmdline "schema.hardening_default=1"
[ $rc -eq 0 ] || fail "drop: exit $rc"
grep -q '^note:.*dropped' "$T/out" || fail "drop: no dropped note"

# cmdline flips the hardening default
newroot hd
svc "$S" x
check --root "$R" --cmdline "schema.hardening_default=0"
has "hardening_default=off" || fail "hd=0: hardening_default"
grep '^x\.no_new_privs=' "$T/out" > "$T/nnp0"
check --root "$R" --cmdline "schema.hardening_default=1"
has "hardening_default=on" || fail "hd=1: hardening_default"
has "cmdline.schema.hardening_default=1" || fail "hd=1: cmdline line"
grep '^x\.no_new_privs=' "$T/out" > "$T/nnp1"
cmp -s "$T/nnp0" "$T/nnp1" && fail "hd: x.no_new_privs did not change"

# the root's hardening-default file, not the live /etc
newroot hdfile
svc "$S" x
printf 'on\n' > "$R/etc/schema-init/hardening-default"
check --root "$R" --cmdline ""
has "hardening_default=on" || fail "hdfile: root's file ignored"
# absent and off in the root must read off whatever the live /etc says
rm "$R/etc/schema-init/hardening-default"
check --root "$R" --cmdline ""
has "hardening_default=off" || fail "hdfile: absent file not off"
printf 'off\n' > "$R/etc/schema-init/hardening-default"
check --root "$R" --cmdline ""
has "hardening_default=off" || fail "hdfile: root's off ignored"

# SCHEMA_SVC_DIR on the cmdline, validated as PID 1 validates it
newroot alt
mkdir -p "$R/alt"; svc "$R/alt" fromalt; svc "$S" fromdefault
check --root "$R" --cmdline "SCHEMA_SVC_DIR=/alt"
has "svc_dir=/alt" || fail "alt: svc_dir"
has "fromalt.exec=" || fail "alt: did not load R/alt"
has "fromdefault.exec=" && fail "alt: loaded the default dir"
has "cmdline.SCHEMA_SVC_DIR=/alt" || fail "alt: cmdline line"
check --root "$R" --cmdline "SCHEMA_SVC_DIR=/nonexistent"
has "svc_dir=/etc/schema-init/services" || fail "alt: invalid dir not refused"
check --root "$R" --cmdline "SCHEMA_SVC_DIR=alt"
has "svc_dir=/etc/schema-init/services" || fail "alt: relative dir not refused"

# --svc-dir overrides, still under the root
check --root "$R" --cmdline "SCHEMA_SVC_DIR=/alt" --svc-dir /etc/schema-init/services
has "svc_dir=/etc/schema-init/services" && has "fromdefault.exec=" || fail "--svc-dir override"

# Pi: the wrapper drops SCHEMA_SVC_DIR and picks known-good on the third failed try
newroot pi
KG=$R/var/lib/schema-init/known-good
mkdir -p "$KG/services" "$R/usr/bin"
svc "$S" cur; svc "$KG/services" kg
printf 'a\n' > "$R/usr/bin/schema-init"; printf 'b\n' > "$KG/schema-init"
mkdir -p "$R/alt"; svc "$R/alt" fromalt
PI="init=/usr/sbin/schema-init-pi SCHEMA_SVC_DIR=/alt"
check --root "$R" --cmdline "$PI"
has "svc_dir=/etc/schema-init/services" && has "next_init=schema-init" || fail "pi: no state should boot current"
printf 'attempts=2\nlast_target=current\n' > "$R/var/lib/schema-init/pi-boot"
check --root "$R" --cmdline "$PI"
has "svc_dir=/var/lib/schema-init/known-good/services" && has "kg.exec=" && has "next_init=known-good" || fail "pi: known-good not chosen"
printf 'attempts=1\nlast_target=current\n' > "$R/var/lib/schema-init/pi-boot"
check --root "$R" --cmdline "$PI"
has "svc_dir=/etc/schema-init/services" || fail "pi: attempts=1 should boot current"
printf 'attempts=3\nlast_target=known-good\n' > "$R/var/lib/schema-init/pi-boot"
check --root "$R" --cmdline "$PI"
[ $rc -eq 0 ] || fail "pi: systemd next boot exit $rc"
[ "$(sed -n '/^svc_dir=/{n;p;}' "$T/out")" = "next_init=systemd" ] || fail "pi: next_init=systemd not after svc_dir"
grep -q '^cur\.' "$T/out" && fail "pi: service lines for a systemd boot"
printf 'attempts=2\nlast_target=current\n' > "$R/var/lib/schema-init/pi-boot"
cp "$KG/schema-init" "$R/usr/bin/schema-init"; rm -rf "$KG/services"; cp -r "$S" "$KG/services"
check --root "$R" --cmdline "$PI"
has "next_init=systemd" || fail "pi: attempts=2 with identical known-good should boot systemd"

# masked is deliberate: a note, never an error
newroot masked
svc "$S" a; svc "$S" m; : > "$S/m.svc.masked"
check --root "$R" --cmdline ""
[ $rc -eq 0 ] || fail "masked: exit $rc"
grep -q '^error:' "$T/out" && fail "masked: error line"
grep -qx 'note: m.svc: masked' "$T/out" || fail "masked: no note"
[ "$(grep -c 'masked' "$T/out")" = 1 ] || fail "masked: loader line repeated"

# the kernel takes the last of a repeated key; words after -- are init's argv
newroot lastkey
mkdir -p "$R/a1" "$R/a2"; svc "$R/a1" one; svc "$R/a2" two
check --root "$R" --cmdline "SCHEMA_SVC_DIR=/a1 SCHEMA_SVC_DIR=/a2"
has "svc_dir=/a2" || fail "lastkey: first SCHEMA_SVC_DIR won"
check --root "$R" --cmdline "SCHEMA_SVC_DIR=/a1 -- SCHEMA_SVC_DIR=/a2"
has "svc_dir=/a1" || fail "lastkey: word after -- read"
has "cmdline.SCHEMA_SVC_DIR=/a2" && fail "lastkey: word after -- in the header"
check --root "$R" --cmdline "root=A root=B"
grep '^cmdline\.root=' "$T/out" > "$T/ab"
check --root "$R" --cmdline "root=B root=A"
grep '^cmdline\.root=' "$T/out" > "$T/ba"
cmp -s "$T/ab" "$T/ba" && fail "lastkey: repeated key order lost"

# kernel quoting: quotes are stripped, a quoted -- is a value, not the end of the options
check --root "$R" --cmdline 'SCHEMA_SVC_DIR="/a2"'
has "svc_dir=/a2" || fail "quoted: SCHEMA_SVC_DIR=\"/a2\" not /a2"
has "cmdline.SCHEMA_SVC_DIR=/a2" || fail "quoted: header kept the quotes"
check --root "$R" --cmdline 'SCHEMA_SVC_DIR=/a1 x="a -- b" SCHEMA_SVC_DIR=/a2 rd.y="p q"'
has "svc_dir=/a2" || fail "quoted: a quoted -- ended the options"
has "cmdline.rd.y=p q" || fail "quoted: quoted word split in the header"
newroot pilast
mkdir -p "$R/var/lib/schema-init/known-good/services"; svc "$S" cur
printf 'attempts=2\nlast_target=current\n' > "$R/var/lib/schema-init/pi-boot"
check --root "$R" --cmdline "init=/usr/sbin/schema-init-pi init=/sbin/schema-init"
has "next_init=" && fail "pilast: first init= taken"
check --root "$R" --cmdline "init=/usr/sbin/schema-init-pi -- init=/sbin/schema-init"
has "next_init=" || fail "pilast: init= after -- taken"

# the root is stripped from every occurrence, even after a longer lookalike
newroot strip
svc "$S" a; printf 'name=w\nexec=/bin/true\nwait_socket=%sx %s/%0120d\n' "$R" "$R" 0 > "$S/w.svc"
check --root "$R" --cmdline ""
grep -qF "$R/" "$T/out" && fail "strip: root left in the output"
has "${R}x /0000" || fail "strip: message mangled"

# a reason is only ever the file's own: another service's " zz: " is not this zz.svc's
newroot attrib
printf 'name=zz\nexec=/bin/true\ndynamic_user=bogus\n' > "$S/a.svc"
svc "$S" dupe; printf 'name=dupe\nexec=/bin/true\n' > "$S/zz.svc"
check --root "$R" --cmdline ""
grep -q '^error: zz\.svc: REFUSED .*zz\.svc: name .dupe.' "$T/out" || fail "attrib: zz.svc not given its own reason"
grep -q '^error: zz\.svc: zz:' "$T/out" && fail "attrib: a.svc's message pinned on zz.svc"

# a refusal that names only the service still reaches its own file, and only its own
newroot ws
printf 'name=zz\nexec=/bin/sleep 1\n' > "$S/a.svc"
printf 'name=zz\nexec=/bin/true\nargs=x\nexec=/bin/echo two words\n' > "$S/zz.svc"
check --root "$R" --cmdline ""
grep -q '^error: a\.svc: zz: exec=/bin/sleep 1 has whitespace' "$T/out" || fail "ws: a.svc reason"
grep -q '^error: zz\.svc: zz: exec=/bin/echo two words has whitespace' "$T/out" || fail "ws: zz.svc reason"
grep -q '^note: .*has whitespace' "$T/out" && fail "ws: reason repeated as a note"

# --svc-dir is a path under the root, so absolute
newroot reldir
svc "$S" a
check --root "$R" --cmdline "" --svc-dir etc/schema-init/services
[ $rc -eq 2 ] || fail "reldir: exit $rc"
grep -q 'svc-dir' "$T/err" || fail "reldir: no message: $(cat "$T/err")"

# a root too long for the loader's 512-byte paths is refused, not every file failed
L=$T/$(printf '%0150d' 0)/$(printf '%0150d' 0)/$(printf '%0150d' 0)
mkdir -p "$L/etc/schema-init/services"; svc "$L/etc/schema-init/services" a
check --root "$L" --cmdline ""
[ $rc -eq 2 ] || fail "longroot: exit $rc"
grep -q 'too long' "$T/err" || fail "longroot: no message: $(cat "$T/err")"
rm -rf "$T/$(printf '%0150d' 0)"

# no fd left for the capture file: an error, not a crash (redirects first, they need fds too).
# Only a sanitizer build may skip: its dynamic loader can't start on 3 fds (exit 127).
# Under qemu-user (deb.yml sets QEMU_SKIP) the emulator itself needs fds and crashes.
newroot nofd
svc "$S" a
sh -c 'exec > /dev/null 2>&1; ulimit -n 3; exec ./schema-init --check'; got=$?
if [ -n "${QEMU_SKIP:-}" ]; then
    echo "check_mode: nofd skipped (qemu-user can't run on 3 fds)"
elif ! { [ $got -eq 127 ] && ldd ./schema-init 2>/dev/null | grep -q 'libasan\|libubsan'; }; then
    sh -c 'exec > "$2" 2> "$3"; ulimit -n 3; exec ./schema-init --check --root "$1" --cmdline ""' sh "$R" "$T/out" "$T/err"
    rc=$?
    [ $rc -eq 2 ] || fail "nofd: exit $rc"
    grep -q 'tmpfile' "$T/err" || fail "nofd: no message: $(cat "$T/err")"
else
    echo "check_mode: nofd skipped (sanitizer build can't start on 3 fds)"
fi

# never binds a socket or makes runtime paths
newroot quiet
svc "$S" a
mkdir -p "$T/xdg"; chmod 0700 "$T/xdg"
find "$T" ! -name before ! -name after ! -name out ! -name err | sort > "$T/before"
XDG_RUNTIME_DIR=$T/xdg/run ./schema-init --check --root "$R" --cmdline "" > /dev/null 2>&1
find "$T" ! -name before ! -name after ! -name out ! -name err | sort > "$T/after"
cmp -s "$T/before" "$T/after" || { diff "$T/before" "$T/after"; fail "quiet: new paths appeared"; }

./schema-init --check > /dev/null 2>&1 && fail "missing --root accepted"

# the shipped baseline oneshot parses through the real loader
newroot baseline_svc
cp services/schema-verify-baseline.svc "$S/"
check --root "$R" --cmdline ""
[ $rc -eq 0 ] || fail "baseline_svc: exit $rc"
has "schema-verify-baseline.exec=" || fail "baseline_svc: no exec line"
# --wait-settled can run 15 min: the oneshot default of 90 s would kill it
has "schema-verify-baseline.start_timeout_sec=0" || fail "baseline_svc: start timeout not 0"
# after the persistent /var, so the write can't land in what it covers
has "schema-verify-baseline.dep=mount-fstab" || fail "baseline_svc: not after mount-fstab"
has "schema-verify-baseline.exec=/bin/sh" || fail "baseline_svc: exec not /bin/sh"
# exit 1/3 are final answers (tmpfs, no --check, never settled): restart=always would loop
has "schema-verify-baseline.restart=no" || fail "baseline_svc: restarts on failure"
# bootok now runs the baseline write inside its own unit, so no 90 s oneshot default either
for f in services/schema-bootok.svc.example distros/fedora-kde/hosts/blakbox/services/schema-bootok.svc; do
    rm -f "$S"/*.svc; cp "$f" "$S/schema-bootok.svc"
    check --root "$R" --cmdline ""
    has "schema-bootok.start_timeout_sec=0" || fail "baseline_svc: $f start timeout not 0"
done
rm -f "$S"/*.svc; cp services/schema-verify-baseline.svc "$S/"
check --root "$R" --cmdline "schema.hardening_default=1"
for k in no_new_privs private_tmp protect_system protect_home; do
    has "schema-verify-baseline.$k=0	src=explicit" || fail "baseline_svc: $k not explicitly 0"
done

# schema-ctl's settle wait: no PID 1 socket is retried quietly, not reported every second.
# A user+pid namespace makes it root with a "schema-init" PID 1 (a copy of sh).
cp /bin/sh "$T/schema-init"
if [ -x ./schema-ctl ] && unshare -r -p -f --mount-proc true 2>/dev/null; then
    unshare -r -p -f --mount-proc "$T/schema-init" -c \
        'timeout 4 ./schema-ctl verify --write-baseline --wait-settled 1 > /dev/null 2> "$1"; true' sh "$T/err"
    [ "$(grep -c . "$T/err")" -eq 0 ] || fail "ctl_quiet: wait printed: $(head -n 4 "$T/err")"
else
    echo "check_mode: ctl_quiet skipped (no user+pid namespace)"
fi

echo "ok check_mode"
