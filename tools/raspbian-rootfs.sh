#!/bin/sh
# Raspbian trixie armhf rootfs (ARMv6 + VFP userland and toolchain) with the
# deb's Build-Depends and what `make test` needs. Run as root on a host with
# qemu-user-static binfmt; Debian's own armhf is ARMv7 and would fail check-armv6.
set -eu
ROOT=${1:?usage: $0 rootfs-dir}
MIRROR=http://raspbian.raspberrypi.com/raspbian
KEY_URL=https://archive.raspbian.org/raspbian.public.key
# Raspbian archive signing key (Mike Thompson, Raspberry Pi Debian armhf ARMv6+VFP).
KEY_FPR=A0DA38D0D76E8B5D638872819165938D90FDDD2E
PKGS=build-essential,debhelper,libzstd-dev,libacl1-dev,pkgconf,libdbus-1-dev,python3,git

K=$(mktemp -d)
trap 'rm -rf "$K"' EXIT
curl -fsSL "$KEY_URL" -o "$K/key.asc"
got=$(GNUPGHOME=$K gpg --show-keys --with-colons "$K/key.asc" | awk -F: '$1 == "pub" { p = 1; next } $1 == "fpr" && p { print $10; p = 0 }')
[ "$got" = "$KEY_FPR" ] || { echo "raspbian key: got '$got', want $KEY_FPR" >&2; exit 1; }
gpg --dearmor < "$K/key.asc" > "$K/raspbian.gpg"

debootstrap --arch=armhf --variant=buildd --keyring="$K/raspbian.gpg" --include="$PKGS" trixie "$ROOT" "$MIRROR"
