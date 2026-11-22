#!/bin/sh
# schema-ctl verify's inspection must never write to the device it reads, even when
# the filesystem has a log or journal waiting to be replayed. Root + loop devices:
# real dirty images (copied while mounted, after an fsync), their bytes compared
# around the inspection. Run with `sudo make verify-nowrite-live`.
set -u
cd "$(dirname "$0")/.."
TD=${TESTDIR:-$PWD/build/test}
BIN=$TD/verify-root
[ "$(id -u)" = 0 ] || { echo "SKIP verify_nowrite_live: needs root"; exit 0; }
[ -x "$BIN" ] || { echo "FAIL verify_nowrite_live: no $BIN (make test first)"; exit 1; }
T=$(mktemp -d -p /var/tmp)
LOOPS=
cleanup() {
    umount "$T/m" 2>/dev/null
    for l in $LOOPS; do losetup -d "$l" 2>/dev/null; done
    rm -rf "$T"
}
trap cleanup EXIT
fail() { echo "FAIL verify_nowrite_live: $*"; exit 1; }
mkdir "$T/m"

# sets $L, not echoed: a $(...) subshell would keep LOOPS from the cleanup trap
attach() { L=$(losetup -f --show "$1") || fail "losetup $1"; LOOPS="$LOOPS $L"; }

# $1 type, $2 name: a clean image and, copied while mounted, a dirty one
mkimg() {
    truncate -s 400M "$T/$2.img"
    "mkfs.$1" -q "$T/$2.img" >/dev/null 2>&1 || "mkfs.$1" "$T/$2.img" >/dev/null 2>&1 || return 1
    l=$(losetup -f --show "$T/$2.img") || return 1
    mount "$l" "$T/m" || { losetup -d "$l"; return 1; }
    mkdir -p "$T/m/sbin" && echo x > "$T/m/sbin/init" && sync &&
        dd if=/dev/urandom of="$T/m/f" bs=1M count=2 conv=fsync status=none &&
        cp --sparse=always "$T/$2.img" "$T/$2-dirty.img"
    r=$?
    umount "$T/m" || fail "umount $2"
    losetup -d "$l"
    return $r
}

set --
for fs in btrfs ext4 xfs; do
    if mkimg "$fs" "$fs" 2>/dev/null; then set -- "$@" "$fs-dirty"; [ "$fs" = xfs ] || set -- "$@" "$fs"
    else echo "skip $fs: no mkfs.$fs or no kernel support"; fi
done
[ $# -gt 0 ] || fail "no image could be built"

for i in "$@"; do sha256sum "$T/$i.img" | cut -d' ' -f1 > "$T/$i.sum"; done
VL_LIVE=1
export VL_LIVE
for i in "$@"; do
    case $i in
        btrfs-dirty) attach "$T/$i.img"; VL_BTRFS_DIRTY=$L; export VL_BTRFS_DIRTY ;;
        ext4-dirty) attach "$T/$i.img"; VL_EXT4_DIRTY=$L; export VL_EXT4_DIRTY ;;
        xfs-dirty) attach "$T/$i.img"; VL_XFS_DIRTY=$L; export VL_XFS_DIRTY ;;
        btrfs) attach "$T/$i.img"; VL_BTRFS_CLEAN=$L; export VL_BTRFS_CLEAN ;;
        ext4) attach "$T/$i.img"; VL_EXT4_CLEAN=$L; export VL_EXT4_CLEAN ;;
    esac
done
# an already-mounted ext4 root: its own copy, mounted read-write as a running root is
if [ -f "$T/ext4.img" ]; then
    cp --sparse=always "$T/ext4.img" "$T/ext4-m.img"
    attach "$T/ext4-m.img"; VL_EXT4_MOUNTED=$L
    export VL_EXT4_MOUNTED
    mount "$VL_EXT4_MOUNTED" "$T/m" || fail "mount ext4-m"
fi

mkdir "$T/unit"
"$BIN" "$T/unit" > "$T/out" 2>&1
rc=$?
grep -E '^(ok live|FAIL|  VL_)|Assert' "$T/out"
[ $rc = 0 ] || fail "verify-root exited $rc"
grep -q '^FAIL' "$T/out" && fail "live case failed"

for l in $LOOPS; do losetup -d "$l" 2>/dev/null; done
LOOPS=
for i in "$@"; do
    [ "$(sha256sum "$T/$i.img" | cut -d' ' -f1)" = "$(cat "$T/$i.sum")" ] || fail "$i.img was written to"
    echo "ok unchanged $i"
done
echo "ok verify_nowrite_live"
