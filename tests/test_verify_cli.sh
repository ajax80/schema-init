#!/bin/sh
# schema-ctl verify end to end, unprivileged: every path from SCHEMA_VERIFY_* env
# to fixtures made here; the mount+chroot inspection replaced by a stub that
# emits root_inspect's records and a canned --check report.
set -u
cd "$(dirname "$0")/.."
TD=${TESTDIR:-$PWD/build/test}
# the SCHEMA_VERIFY_* overrides exist only in a -DSCHEMA_VERIFY_TESTHOOKS build
CTL=${1:-$TD/schema-ctl-testhooks}
[ -x "$CTL" ] || { echo "FAIL verify_cli: no $CTL"; exit 1; }
[ -x ./schema-ctl ] || { echo "FAIL verify_cli: no ./schema-ctl"; exit 1; }
mkdir -p "$TD"
# under TESTDIR, not /tmp: the state dir refuses a tmpfs
T=$TD/vc
rm -rf "$T"
mkdir -p "$T"
trap 'chmod -R u+rwx "$T" 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$T"' EXIT
umask 022
fail() { echo "FAIL verify_cli: $*"; [ -f "$T/out" ] && sed 's/^/  | /' "$T/out"; exit 1; }

PU=3e078549-8379-4ae4-9ae3-bacc2ea36956
mkdir -p "$T/dev/disk/by-partuuid" "$T/dev/disk/by-uuid" "$T/bls" "$T/state" "$T/fx" "$T/esp/EFI/fedora"
: > "$T/dev/nvme0n1p3"
ln -s ../../nvme0n1p3 "$T/dev/disk/by-partuuid/$PU"
ln -s ../../nvme0n1p3 "$T/dev/disk/by-uuid/aaaa-1111"
cp tests/fixtures/verify/inspect-stub.sh "$T/inspect"
chmod 755 "$T/inspect"

entry() { # id subvol [extra option words]
    printf 'title %s\nversion 7.2.11\nlinux /%s/boot/vmlinuz-7.2.11\noptions root=PARTUUID=%s ro rootflags=subvol=%s init=/sbin/schema-init%s\ngrub_class fedora\n' \
        "$1" "$2" "$PU" "$2" "${3:+ $3}" > "$T/bls/$1.conf"
}
entry m-7.2.11 root
entry schema-snap-20261118-175630-post-bootfix schema-snap-20261118-175630-post-bootfix
entry schema-snap-20261011-200415-post-update schema-snap-20261011-200415-post-update
printf 'saved_entry=m-7.2.11\nschema_fallback_id=schema-snap-20261118-175630-post-bootfix\n' > "$T/grubenv"
cp tests/fixtures/verify/boot/grub-real.cfg "$T/grub.cfg"
printf 'search --no-floppy --fs-uuid --set=dev 1\nset prefix=($dev)/root/boot/grub2\nconfigfile $prefix/grub.cfg\n' > "$T/esp/EFI/fedora/grub.cfg"
printf 'UUID=aaaa-1111 / btrfs subvol=root 0 0\ntmpfs /tmp tmpfs defaults 0 0\n' > "$T/fstab"
: > "$T/kmsg"

HDR="schema-verify-baseline v1
binary_version=0.4.29
cmdline.init=/sbin/schema-init
cmdline.root=PARTUUID=$PU
cmdline.rootflags=subvol=root
hardening_default=on
svc_dir=/etc/schema-init/services"
report() { # file, then service lines
    f=$1; shift
    { printf '%s\n' "$HDR"; printf '%s\n' "$@"; } > "$f"
}
GOOD="a.exec=/bin/true
schema-bootok.exec=/usr/bin/schema-bootok"
report "$T/fx/check.out" "$GOOD"
report "$T/state/baseline" "$GOOD"
: > "$T/state/established"
chmod 700 "$T/state"

export SCHEMA_VERIFY_DIR="$T/state" SCHEMA_VERIFY_GRUBENV="$T/grubenv" SCHEMA_VERIFY_BLS_DIR="$T/bls" \
    SCHEMA_VERIFY_GRUBCFG="$T/grub.cfg" SCHEMA_VERIFY_FSTAB="$T/fstab" SCHEMA_VERIFY_KMSG="$T/kmsg" \
    SCHEMA_VERIFY_DEVDIR="$T/dev" SCHEMA_VERIFY_ESP="$T/esp" SCHEMA_VERIFY_INSPECT="$T/inspect" \
    SCHEMA_VERIFY_CMDLINE_TXT="$T/no-cmdline.txt" VFX="$T/fx"

v() { "$CTL" verify "$@" > "$T/out" 2>&1; rc=$?; }
has() { grep -qF -- "$1" "$T/out"; }
expect() { [ "$rc" -eq "$1" ] || fail "$2: exit $rc, want $1"; }

# clean
v
expect 0 clean
has "Result: 0 fail, 0 change, 0 warn" || fail "clean: result line"
for s in "Next boot" Services "Ways back" Disks; do grep -qx "$s" "$T/out" || fail "clean: no section $s"; done
has "[PASS] actual next boot m-7.2.11" || fail "clean: actual entry"
has "[PASS] guard fallback schema-snap-20261118-175630-post-bootfix" || fail "clean: fallback entry"
has "[PASS] 09_schema_fallback is live" || fail "clean: live fallback"
has "[PASS] schema-bootok" || fail "clean: bootok"
grep -q '^\[FAIL\]\|^\[UNRESOLVED\]\|^\[CHANGE' "$T/out" && fail "clean: non-pass line"
# sections in the spec's order
[ "$(grep -x 'Next boot\|Services\|Ways back\|Disks' "$T/out" | tr '\n' ,)" = "Next boot,Services,Ways back,Disks," ] ||
    fail "clean: section order"

# --quiet: findings and the result only
v --quiet
expect 0 quiet
grep -q '^\[PASS\]\|^\[note\]\|^Next boot$' "$T/out" && fail "quiet: PASS/note/heading shown"
[ "$(wc -l < "$T/out")" -eq 1 ] && has "Result:" || fail "quiet: want only the result line"

# unaccepted CHANGE -> 2; --accept -> 0, [CHANGE accepted]; an edit after -> 2
report "$T/fx/check.out" "a.exec=/bin/false" "schema-bootok.exec=/usr/bin/schema-bootok"
v
expect 2 change
has "[CHANGE] exec  /bin/true → /bin/false" || fail "change: line"
has "1 change (not accepted)" || fail "change: result"
v --quiet
expect 2 change-quiet
has "[CHANGE] exec" || fail "change-quiet: CHANGE hidden"
v --accept
expect 0 accept
has "[CHANGE accepted] exec" && has "1 change (accepted)" || fail "accept: tag"
[ "$(stat -c %a "$T/state/accepted")" = 600 ] || fail "accept: mode"
v
expect 0 accepted-rerun
has "[CHANGE accepted]" || fail "accepted-rerun: tag"
report "$T/fx/check.out" "a.exec=/bin/sleep" "schema-bootok.exec=/usr/bin/schema-bootok"
v
expect 2 edit-after-accept
has "[CHANGE] exec  /bin/true → /bin/sleep" || fail "edit-after-accept: line"
report "$T/fx/check.out" "$GOOD"
rm -f "$T/state/accepted"

# a FAIL is never accepted
report "$T/fx/check.out" "a.exec=/bin/false" "schema-bootok.exec=/usr/bin/schema-bootok"
printf 'UUID=gone-0000 /home btrfs defaults 0 0\n' >> "$T/fstab"
v --accept
expect 1 accept-with-fail
[ -e "$T/state/accepted" ] && fail "accept-with-fail: accepted written"
has "[FAIL] /home — required btrfs device UUID=gone-0000 not present" || fail "accept-with-fail: disk line"
printf 'UUID=aaaa-1111 / btrfs subvol=root 0 0\ntmpfs /tmp tmpfs defaults 0 0\n' > "$T/fstab"
report "$T/fx/check.out" "$GOOD"

# blank line before the fallback entry's last key: GRUB drops the rest -> 1
F=schema-snap-20261118-175630-post-bootfix
printf 'title x\nlinux /%s/boot/vmlinuz-7.2.11\ninitrd\noptions root=PARTUUID=%s ro rootflags=subvol=%s init=/sbin/schema-init\n' \
    "$F" "$PU" "$F" > "$T/bls/$F.conf"
v
expect 1 blank-fallback
has "[FAIL] guard fallback $F: blank line" || fail "blank-fallback: line"
v --quiet
has "[FAIL] guard fallback $F" || fail "blank-fallback: quiet"
entry "$F" "$F"

# an older snapshot entry broken: WARN only
printf 'title x\nlinux /y/boot/vmlinuz\n\noptions root=PARTUUID=%s\n' "$PU" > "$T/bls/schema-snap-20261011-200415-post-update.conf"
v
expect 0 other-snap
has "[WARN] schema-snap-20261011-200415-post-update: blank line" || fail "other-snap: line"
entry schema-snap-20261011-200415-post-update schema-snap-20261011-200415-post-update

# schema_fallback_id naming no entry -> 1
printf 'saved_entry=m-7.2.11\nschema_fallback_id=schema-snap-nope\n' > "$T/grubenv"
v
expect 1 fallback-unknown
has "[FAIL] schema_fallback_id=schema-snap-nope names no boot entry" || fail "fallback-unknown: line"
# guard exhausted with no fallback id: GRUB boots a menu position -> UNRESOLVED
printf 'saved_entry=m-7.2.11\nboot_counter=0\nboot_success=0\n' > "$T/grubenv"
v
expect 3 exhausted
has "[UNRESOLVED]" || fail "exhausted: no UNRESOLVED"
# next_entry: the one-shot is the actual next boot, saved_entry the normal one
printf 'saved_entry=m-7.2.11\nnext_entry=%s\nschema_fallback_id=%s\n' "$F" "$F" > "$T/grubenv"
v
expect 0 next-entry
has "[PASS] actual next boot $F" && has "[PASS] normal default m-7.2.11" || fail "next-entry: entries"
printf 'saved_entry=m-7.2.11\nschema_fallback_id=%s\n' "$F" > "$T/grubenv"

# guard can't fire: fallback only in a comment -> 1
cp tests/fixtures/verify/boot/grub-comment.cfg "$T/grub.cfg"
v
expect 1 grubcfg-comment
has "[FAIL] 09_schema_fallback is not live" || fail "grubcfg-comment: line"
cp tests/fixtures/verify/boot/grub-real.cfg "$T/grub.cfg"

# a full GRUB config on the ESP loads its own grubenv -> 1
printf '### BEGIN /etc/grub.d/00_header ###\nset default=0\n' > "$T/esp/EFI/fedora/grub.cfg"
v
expect 1 esp-full
has "[FAIL] ESP /EFI/fedora/grub.cfg is a full GRUB config" || fail "esp-full: line"
printf 'configfile $prefix/grub.cfg\n' > "$T/esp/EFI/fedora/grub.cfg"

# schema-bootok: not on the next boot's rail; refused; exec not executable -> 1
report "$T/fx/check.out" "a.exec=/bin/true"
report "$T/state/baseline" "a.exec=/bin/true"
v
expect 1 bootok-missing
has "[FAIL] schema-bootok is not installed" || fail "bootok-missing: line"
report "$T/fx/check.out" "error: schema-bootok.svc: protect_home hides exec — not loaded" "a.exec=/bin/true"
v
expect 1 bootok-refused
has "[FAIL] schema-bootok.svc does not load on the next boot" || fail "bootok-refused: line"
report "$T/fx/check.out" "$GOOD"
report "$T/state/baseline" "$GOOD"
printf 2 > "$T/fx/bootok"
v
expect 1 bootok-noexec
has "[FAIL] schema-bootok exec=/usr/bin/schema-bootok is not executable" || fail "bootok-noexec: line"
rm "$T/fx/bootok"

# not armed and no guard anywhere: ways back noted, never FAIL
printf 'saved_entry=m-7.2.11\n' > "$T/grubenv"
cp tests/fixtures/verify/boot/grub-comment.cfg "$T/grub.cfg"
grep -v schema_fallback "$T/grub.cfg" > "$T/g2" && mv "$T/g2" "$T/grub.cfg"
mkdir "$T/bls-off"
mv "$T/bls/schema-snap-"* "$T/bls-off/"
v
expect 0 no-guard
has "[note] boot guard not in use" || fail "no-guard: note"
mv "$T/bls-off/"* "$T/bls/"
rmdir "$T/bls-off"
cp tests/fixtures/verify/boot/grub-real.cfg "$T/grub.cfg"
printf 'saved_entry=m-7.2.11\nschema_fallback_id=%s\n' "$F" > "$T/grubenv"

# BASE_LOST: marker without a baseline -> 2, and --accept can't clear it
mv "$T/state/baseline" "$T/state/baseline.off"
v
expect 2 base-lost
has "[CHANGE] baseline lost" || fail "base-lost: line"
v --accept
expect 2 base-lost-accept
[ -e "$T/state/accepted" ] && fail "base-lost-accept: accepted written"
mv "$T/state/baseline.off" "$T/state/baseline"

# never established: WARN, never blocks
mv "$T/state/established" "$T/state/established.off"
v
expect 0 base-never
has "[WARN] no baseline yet" || fail "base-never: line"
mv "$T/state/established.off" "$T/state/established"

# a baseline from an older format: diff_baseline's WARN
sed '1s/v1/v0/' "$T/state/baseline" > "$T/b0" && mv "$T/b0" "$T/state/baseline"
v
expect 0 base-old
has "[WARN] baseline from an older format" || fail "base-old: line"
report "$T/state/baseline" "$GOOD"

# --check errors on the next boot that the baseline did not have -> 1
report "$T/fx/check.out" "error: b.svc: dynamic_user=bogus" "$GOOD"
printf 1 > "$T/fx/rc-m-7.2.11"
v
expect 1 check-error
has "[FAIL] b.svc: dynamic_user=bogus" || fail "check-error: line"
rm "$T/fx/rc-m-7.2.11"
report "$T/fx/check.out" "$GOOD"

# init= not schema-init: a systemd next boot
printf /usr/lib/systemd/systemd > "$T/fx/init-m-7.2.11"
v
expect 0 systemd
has "[PASS] next boot is /usr/lib/systemd/systemd; services not checked" || fail "systemd: line"
rm "$T/fx/init-m-7.2.11"

# root_inspect's own unresolved line, as it would come from the child
printf 'U inspection did not finish within 30 s\n' > "$T/fx/ctl-m-7.2.11"
v
expect 3 inspect-unres
has "[UNRESOLVED] actual next boot m-7.2.11: inspection did not finish within 30 s" || fail "inspect-unres: line"
rm "$T/fx/ctl-m-7.2.11"

# disks: an I/O error in the kernel log is a WARN
printf '6,100,5,-;blk_update_request: I/O error, dev sdb, sector 8\n' > "$T/kmsg"
v
expect 0 kmsg
has "[WARN] sdb — 1 I/O error in the kernel log since boot" || fail "kmsg: line"
: > "$T/kmsg"

# no grubenv on a box that is not a Pi: unresolved, never PASS
mv "$T/grubenv" "$T/grubenv.off"
v
expect 3 no-grubenv
has "[UNRESOLVED] grubenv $T/grubenv" || fail "no-grubenv: line"
grep -q '^\[PASS\] actual' "$T/out" && fail "no-grubenv: PASS for the next boot"

# a Pi: no grubenv, cmdline.txt; services from schema-init --check on it
printf 'console=tty1 root=PARTUUID=%s rootwait init=/usr/sbin/schema-init-pi\n' "$PU" > "$T/cmdline.txt"
printf '#!/bin/sh\necho "$@" > "%s/pi-args"\ncat "%s/fx/check.out"\n' "$T" "$T" > "$T/pi-init"
chmod 755 "$T/pi-init"
export SCHEMA_VERIFY_CMDLINE_TXT="$T/cmdline.txt" SCHEMA_VERIFY_PI_INIT="$T/pi-init"
v
expect 0 pi
has "[note] boot entries: not applicable" || fail "pi: boot note"
has "[PASS] services match the baseline" || fail "pi: services"
grep -qF -- "--check --root / --cmdline console=tty1 root=PARTUUID=$PU rootwait init=/usr/sbin/schema-init-pi" "$T/pi-args" ||
    fail "pi: --check args: $(cat "$T/pi-args")"
if [ "$(id -u)" != 0 ]; then
    # unprivileged, a 0700 state dir hides the baseline: that is not "no baseline yet"
    chmod 000 "$T/state"
    v
    expect 3 pi-nonroot-baseline
    has "[UNRESOLVED] needs root: baseline" || fail "pi-nonroot-baseline: line"
    has "no baseline yet" && fail "pi-nonroot-baseline: read as never established"
    chmod 700 "$T/state"
fi
printf '#!/bin/sh\nexit 2\n' > "$T/pi-init"
v
expect 0 pi-undetermined
has "[note] services: not applicable" || fail "pi-undetermined: note"
mv "$T/grubenv.off" "$T/grubenv"
export SCHEMA_VERIFY_CMDLINE_TXT="$T/no-cmdline.txt"
unset SCHEMA_VERIFY_PI_INIT

# usage errors are 64, never 1: a gate must not read them as a FAIL
v --bogus
expect 64 usage
v --write-baseline --wait-settled x
expect 64 usage-wait

# saved_entry unset: GRUB boots the first menu entry, the newest kernel
mkdir "$T/bls2"
for k in 7.2.9 7.2.11 7.2.10; do
    printf 'title k\nlinux /root/boot/vmlinuz-%s\noptions root=PARTUUID=%s ro rootflags=subvol=root init=/sbin/schema-init\n' \
        "$k" "$PU" > "$T/bls2/m-$k.conf"
done
printf 'blsdir=/x\n' > "$T/grubenv2"
SCHEMA_VERIFY_BLS_DIR="$T/bls2" SCHEMA_VERIFY_GRUBENV="$T/grubenv2" v
has "[PASS] actual next boot m-7.2.11:" || fail "newest: actual is not m-7.2.11"

# schema-good* entries alone mean the guard is in use
cp tests/fixtures/verify/boot/grub-comment.cfg "$T/grub.cfg"
grep -v schema_fallback "$T/grub.cfg" > "$T/g2" && mv "$T/g2" "$T/grub.cfg"
printf 'saved_entry=m-7.2.11\n' > "$T/grubenv"
mkdir "$T/bls-off"
mv "$T/bls/schema-snap-"* "$T/bls-off/"
entry schema-good-20260915 schema-good-20260915
v
expect 1 good-guard
has "[FAIL] 09_schema_fallback is not live" || fail "good-guard: guard not judged"
rm "$T/bls/schema-good-20260915.conf"
mv "$T/bls-off/"* "$T/bls/"
rmdir "$T/bls-off"
cp tests/fixtures/verify/boot/grub-real.cfg "$T/grub.cfg"
printf 'saved_entry=m-7.2.11\nschema_fallback_id=%s\n' "$F" > "$T/grubenv"

# more entries than verify reads: unresolved, not a silent cut
mkdir "$T/bls3"
cp "$T/bls/"*.conf "$T/bls3/"
i=0
while [ $i -lt 70 ]; do printf 'title x\nlinux /x\noptions root=PARTUUID=%s\n' "$PU" > "$T/bls3/z-$i.conf"; i=$((i + 1)); done
SCHEMA_VERIFY_BLS_DIR="$T/bls3" v
has "[UNRESOLVED] too many boot entries" || fail "many: line"

if [ "$(id -u)" != 0 ]; then
    # an entry file that can't be read: a spare is a WARN; the guard's fallback is unresolved, not "no entry"
    chmod 000 "$T/bls/schema-snap-20261011-200415-post-update.conf"
    v
    has "[WARN] boot entry $T/bls/schema-snap-20261011-200415-post-update.conf could not be read" || fail "unreadable-spare: line"
    chmod 644 "$T/bls/schema-snap-20261011-200415-post-update.conf"
    chmod 000 "$T/bls/$F.conf"
    v
    expect 3 unreadable-fallback
    has "[UNRESOLVED] boot entry $T/bls/$F.conf could not be read" || fail "unreadable-fallback: line"
    has "names no boot entry" && fail "unreadable-fallback: called missing"
    chmod 644 "$T/bls/$F.conf"
fi

# the shipped binary has no hooks: every SCHEMA_VERIFY_* path above is ignored
./schema-ctl verify > "$T/out" 2>&1
grep -qF "$T" "$T/out" && fail "shipped schema-ctl used a SCHEMA_VERIFY_* path"
printf 'UUID=hook-only-0000 /hook btrfs defaults 0 0\n' > "$T/fstab-hook"
SCHEMA_VERIFY_FSTAB="$T/fstab-hook" ./schema-ctl verify > "$T/out" 2>&1
grep -q "hook-only" "$T/out" && fail "shipped schema-ctl read SCHEMA_VERIFY_FSTAB"
SCHEMA_VERIFY_FSTAB="$T/fstab-hook" "$CTL" verify > "$T/out" 2>&1
grep -q "hook-only" "$T/out" || fail "testhooks build ignored SCHEMA_VERIFY_FSTAB"

# unprivileged with the real paths: unresolved, exit 3, no crash
if [ "$(id -u)" != 0 ]; then
    env -u SCHEMA_VERIFY_DIR -u SCHEMA_VERIFY_GRUBENV -u SCHEMA_VERIFY_BLS_DIR -u SCHEMA_VERIFY_GRUBCFG \
        -u SCHEMA_VERIFY_FSTAB -u SCHEMA_VERIFY_KMSG -u SCHEMA_VERIFY_DEVDIR -u SCHEMA_VERIFY_ESP \
        -u SCHEMA_VERIFY_INSPECT -u SCHEMA_VERIFY_CMDLINE_TXT "$CTL" verify > "$T/out" 2>&1
    rc=$?
    expect 3 non-root
    has "[UNRESOLVED]" || fail "non-root: no UNRESOLVED"
    # where the real boot files exist, unreadable ones are named for what they need
    if [ -e /boot/grub2/grubenv ] && ! [ -r /boot/grub2/grubenv ]; then
        has "[UNRESOLVED] needs root" || fail "non-root: needs root"
    fi
    grep -q '^\[PASS\] \(actual\|normal\|services\)' "$T/out" && fail "non-root: PASS for an unread input"
fi

echo "verify_cli: ok"
