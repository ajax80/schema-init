#!/bin/sh
# Fails unless every ELF in the given .debs is ARMv6 (Tag_CPU_arch v6 or v6KZ),
# so an armhf build that picked up a v7 toolchain never reaches a Pi Zero/1.
set -eu
[ $# -gt 0 ] || { echo "usage: $0 file.deb..." >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
bad=0
n=0
for deb; do
    rm -rf "$T/x"
    mkdir "$T/x"
    dpkg-deb -x "$deb" "$T/x"
    find "$T/x" -type f > "$T/list"
    while IFS= read -r f; do
        [ "$(od -An -tx1 -N4 "$f" | tr -d ' \n')" = 7f454c46 ] || continue
        n=$((n + 1))
        tag=$(readelf -A "$f" 2>/dev/null | sed -n 's/^ *Tag_CPU_arch: *//p' | head -1)
        case $tag in
        v6 | v6KZ) ;;
        *) echo "$deb: ${f#"$T/x"}: Tag_CPU_arch=${tag:-none}"; bad=1 ;;
        esac
    done < "$T/list"
done
[ "$n" -gt 0 ] || { echo "no ELF files in $*"; exit 1; }
[ "$bad" = 0 ] || exit 1
echo "ok: $n ELF files, all ARMv6"
