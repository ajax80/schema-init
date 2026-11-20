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

echo "ok check_mode"
