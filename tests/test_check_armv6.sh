#!/bin/sh
# tools/check-armv6.sh gate. Needs a Debian container with an ARM cross compiler,
# so it is not part of `make test`. Run from the repo root:
#   podman run --rm -v "$PWD":/src:Z -w /src debian:trixie sh -c \
#     'apt-get update -q && apt-get install -yq dpkg-dev gcc gcc-arm-linux-gnueabihf && sh tests/test_check_armv6.sh'
# Extra args check real debs: pass:/path/x_armhf.deb fail:/path/x_amd64.deb
set -u
cd "$(dirname "$0")/.."
CHECK=$PWD/tools/check-armv6.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

mkdeb() {
    d=$T/deb/$1; shift
    mkdir -p "$d/DEBIAN" "$d/usr/bin"
    printf 'Package: t\nVersion: 1\nArchitecture: all\nMaintainer: t <t@t>\nDescription: t\n' > "$d/DEBIAN/control"
    printf '#!/bin/sh\n' > "$d/usr/bin/script"
    i=0
    for elf; do i=$((i+1)); cp "$elf" "$d/usr/bin/prog$i"; done
    dpkg-deb -b --root-owner-group "$d" "$d.deb" >/dev/null || fail "dpkg-deb $d"
}
expect() {
    sh "$CHECK" "$2" > "$T/out" 2>&1
    rc=$?
    sed 's/^/    /' "$T/out"
    [ "$rc" = "$1" ] || fail "$2: exit $rc, want $1"
    [ -z "${3:-}" ] || grep -q -- "$3" "$T/out" || fail "$2: output lacks '$3'"
    echo "ok: $2 -> $rc"
}

printf 'int main(void){return 0;}\n' > "$T/m.c"
gcc "$T/m.c" -o "$T/host" || fail "host gcc"
arm-linux-gnueabihf-gcc -march=armv7-a+fp "$T/m.c" -o "$T/v7" || fail "v7 cross gcc"
# Object, not executable: Debian's armhf crt files are v7 and would raise the tag.
arm-linux-gnueabihf-gcc -c -marm -march=armv6+fp -mfloat-abi=hard "$T/m.c" -o "$T/v6.o" || fail "v6 cross gcc"

mkdeb host "$T/host"
mkdeb v7 "$T/v7"
mkdeb v6 "$T/v6.o"
mkdeb mixed "$T/v6.o" "$T/v7"
mkdeb noelf

expect 1 "$T/deb/host.deb" "usr/bin/prog1: Tag_CPU_arch=none"
expect 1 "$T/deb/v7.deb" "usr/bin/prog1: Tag_CPU_arch=v7"
expect 0 "$T/deb/v6.deb"
expect 1 "$T/deb/mixed.deb" "usr/bin/prog2: Tag_CPU_arch=v7"
expect 1 "$T/deb/noelf.deb" "no ELF"
sh "$CHECK" "$T/deb/v6.deb" "$T/deb/noelf.deb" > "$T/out" 2>&1 && fail "v6+noelf passed"
grep -q "no ELF files in $T/deb/noelf.deb" "$T/out" || fail "v6+noelf: no per-deb message"
echo "ok: v6.deb + noelf.deb -> 1"

for a; do
    case $a in
    pass:*) expect 0 "${a#pass:}" ;;
    fail:*) expect 1 "${a#fail:}" ;;
    *) fail "bad arg $a" ;;
    esac
done
echo "test_check_armv6: all ok"
