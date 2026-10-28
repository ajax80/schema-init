#!/bin/sh
set -u
cd "$(dirname "$0")/.."
SRC=$PWD
BEFORE=$(cksum "$SRC/debian/changelog" 2>/dev/null)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

V=$(sed -n 's/^Version:[[:space:]]*//p' schema-init.spec | head -1)
[ -n "$V" ] || fail "no spec Version"

# Temp copy so the generated debian/changelog never lands in the tree.
mkdir -p "$T/w/tools" "$T/w/debian"
cp tools/deb-changelog.sh "$T/w/tools/"
cp schema-init.spec "$T/w/"
cd "$T/w" && git init -q . && git add -A >/dev/null 2>&1 &&
    git -c user.name=t -c user.email=t@t commit -qm t || fail "temp repo"

S=$(sh tools/deb-changelog.sh stable) || fail "stable run"
[ "$S" = "$V-1" ] || fail "stable: $S != $V-1"
head -1 debian/changelog | grep -q "^schema-init ($V-1) stable; urgency=medium" || fail "stable changelog header"
grep -q ' -- Jonathan Ayers <ayersjon80@gmail.com>  ' debian/changelog || fail "maintainer line"

D=$(sh tools/deb-changelog.sh dev) || fail "dev run"
echo "$D" | grep -Eq "^$(echo "$V" | sed 's/\./\\./g')-1\.[0-9]+\.[0-9]{8}git[0-9a-f]{7}\$" || fail "dev regex: $D"
head -1 debian/changelog | grep -q "^schema-init ($D) dev; urgency=medium" || fail "dev changelog header"

N=$(echo "$V" | awk -F. -v OFS=. '{$NF=$NF+1; print}')-1
if command -v dpkg >/dev/null 2>&1; then
    dpkg --compare-versions "$D" gt "$S" || fail "dev !gt stable"
    dpkg --compare-versions "$N" gt "$D" || fail "next stable !gt dev"
else
    echo "skipped dpkg compare (no dpkg)"
fi
[ "$(cksum "$SRC/debian/changelog" 2>/dev/null)" = "$BEFORE" ] || fail "changelog leaked into tree"
echo ok test_deb_version
