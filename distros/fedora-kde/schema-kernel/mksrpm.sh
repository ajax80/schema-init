#!/bin/bash
# Turn a bieszczaders/kernel-cachyos SRPM into kernel-cachyos-schema: the same
# kernel with root storage built in, so it boots with no initramfs and hands
# straight to schema-init (~1.5 s faster to first service on blakbox, 11-24).
#
#   mksrpm.sh <kernel-cachyos-*.src.rpm> [outdir]   -> outdir/kernel-cachyos-schema-*.src.rpm
#
# The delta is edits to upstream's spec, not a forked spec, so each new cachyos
# release only needs this re-run. Every edit is checked: if upstream's spec
# moves and a pattern stops matching, this fails instead of building a stock
# kernel under our name.
set -euo pipefail

[ $# -ge 1 ] || { echo "usage: mksrpm.sh <kernel-cachyos-*.src.rpm> [outdir]" >&2; exit 2; }
SRC="$(realpath "$1")"
OUT="$(realpath "${2:-.}")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/SOURCES" "$WORK/SPECS"
(cd "$WORK/SOURCES" && rpm2cpio "$SRC" | cpio -idm --quiet)
specs=("$WORK"/SOURCES/*.spec)
[ ${#specs[@]} -eq 1 ] || { echo "mksrpm: expected one spec in $SRC, found ${#specs[@]}" >&2; exit 1; }
mv "${specs[0]}" "$WORK/SPECS/upstream.spec"
SPEC="$WORK/SPECS/kernel-cachyos-schema.spec"
cp "$WORK/SPECS/upstream.spec" "$SPEC"

edit() {
    # $1 = sed expression, $2 = what it must leave in the spec
    sed -i -e "$1" "$SPEC"
    grep -qF -- "$2" "$SPEC" || { echo "mksrpm: upstream spec changed, edit did not apply: $1" >&2; exit 1; }
}

# uname -r: <ver>-<rel>.cachyos1.schema.fc44.x86_64. ".schema" is what
# 99-schema-init.install keys on to drop the initrd and force root=PARTUUID=;
# it goes in Release because rpm allows one '-' in a uname-r Provides.
edit 's/^\(Release: *%{_cachyos_release}\.cachyos1\)%{?_lto_args:\.lto}/\1.schema/' \
     'Release:        %{_cachyos_release}.cachyos1.schema'
# a wrong uname would install modules where the kernel never looks
# shellcheck disable=SC2016 # $(...) is for the spec, not this shell
edit 's/^\(    %make_build EXTRAVERSION=-%{release}\.%{_arch} all\)$/\1\n    test "$(make -s EXTRAVERSION=-%{release}.%{_arch} kernelrelease)" = "%{_kver}"/' \
     'kernelrelease)" = "%{_kver}"'

edit 's/^Name:\( *\)kernel-cachyos%{?_lto_args:-lto}$/Name:\1kernel-cachyos-schema/' \
     'kernel-cachyos-schema'
# upstream's upgrade path from its old naming must not pull this package in
sed -i -e '/^Provides: *kernel-cachyos%{?_lto_args:-lto} >/d' -e '/^Obsoletes: *kernel-cachyos/d' "$SPEC"
# every package must derive from the new Name: a hardcoded upstream name would
# clash with stock kernel-cachyos or stand in for it
if grep -nE '^(Provides|Obsoletes|Requires|Conflicts):.*kernel-cachyos[^-]|^(Provides|Obsoletes|Requires|Conflicts):.*kernel-cachyos$|^%package +-n +kernel-cachyos' "$SPEC"; then
    echo "mksrpm: upstream spec still names kernel-cachyos directly (above)" >&2; exit 1
fi

# everything on the path to root must be built in: NVMe + btrfs (upstream has
# btrfs =y today; forcing it keeps a config change from making it a module)
edit 's/^\(    scripts\/config -u DEFAULT_HOSTNAME\)$/\1\n\n    # schema: boot with no initramfs (root is btrfs on NVMe)\n    scripts\/config -e BLK_DEV_NVME -e NVME_CORE -e BTRFS_FS -d LOCALVERSION_AUTO/' \
     'scripts/config -e BLK_DEV_NVME -e NVME_CORE -e BTRFS_FS -d LOCALVERSION_AUTO'
# olddefconfig may refuse a symbol; check what it produced. One grep per line:
# %prep runs under sh -e, which ignores a failure on the left of &&
edit 's/^\(    diff -u %{SOURCE1} \.config || :\)$/\1\n    grep -qx CONFIG_BLK_DEV_NVME=y .config\n    grep -qx CONFIG_NVME_CORE=y .config\n    grep -qx CONFIG_BTRFS_FS=y .config/' \
     'grep -qx CONFIG_BTRFS_FS=y .config'

rpmbuild -bs --define "_topdir $WORK" "$SPEC" >/dev/null
mkdir -p "$OUT"
cp "$WORK"/SRPMS/kernel-cachyos-schema-*.src.rpm "$OUT/"
ls "$OUT"/kernel-cachyos-schema-*.src.rpm
