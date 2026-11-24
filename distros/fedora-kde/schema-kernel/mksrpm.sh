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

SRC="$(realpath "$1")"
OUT="$(realpath "${2:-.}")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/SOURCES" "$WORK/SPECS"
(cd "$WORK/SOURCES" && rpm2cpio "$SRC" | cpio -idm --quiet)
mv "$WORK/SOURCES/kernel-cachyos.spec" "$WORK/SPECS/upstream.spec"
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

edit 's/^\(    scripts\/config -u DEFAULT_HOSTNAME\)$/\1\n\n    # schema: boot with no initramfs (root is on NVMe)\n    scripts\/config -e BLK_DEV_NVME -e NVME_CORE -d LOCALVERSION_AUTO/' \
     'scripts/config -e BLK_DEV_NVME -e NVME_CORE -d LOCALVERSION_AUTO'
# olddefconfig may refuse a symbol; check what it actually produced
edit 's/^\(    diff -u %{SOURCE1} \.config || :\)$/\1\n    grep -qx CONFIG_BLK_DEV_NVME=y .config \&\& grep -qx CONFIG_NVME_CORE=y .config/' \
     'grep -qx CONFIG_BLK_DEV_NVME=y .config'

rpmbuild -bs --define "_topdir $WORK" "$SPEC" >/dev/null
mkdir -p "$OUT"
cp "$WORK"/SRPMS/kernel-cachyos-schema-*.src.rpm "$OUT/"
ls "$OUT"/kernel-cachyos-schema-*.src.rpm
