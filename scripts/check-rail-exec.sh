#!/bin/sh
set -eu
svcd=$1 ks=$2
shift 2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for r in "$@"; do rpm -qlp "$r"; done | sort -u > "$tmp/rpmfiles"
sed -n 's|^rm -f /etc/schema-init/services/\([^ ]*\).*|\1|p' "$ks" > "$tmp/dropped"

bad=0 n=0
for f in "$svcd"/*.svc; do
    b=${f##*/}
    skip=
    while read -r pat; do
        case "$b" in $pat) skip=1 ;; esac
    done < "$tmp/dropped"
    [ -n "$skip" ] && continue
    t=$(sed -n 's/^exec=//p' "$f" | tail -1)
    [ -n "$t" ] || continue
    n=$((n + 1))
    grep -qxF "$t" "$tmp/rpmfiles" && continue
    grep -qE "^[^#]*install [^#]*[[:space:]]$t([[:space:]]|$)" "$ks" && continue
    u=$(printf '%s' "$t" | sed 's|^/s\{0,1\}bin/|/usr/bin/|; s|^/usr/sbin/|/usr/bin/|')
    [ -n "$(dnf -q repoquery --repo=fedora --repo=updates --setopt=optional_metadata_types=filelists \
            --whatprovides "$u" 2>/dev/null)" ] && continue
    echo "  $b: exec=$t — no payload RPM, kickstart install or Fedora package ships it" >&2
    bad=1
done
[ "$bad" = 0 ] || { echo "rail exec check FAILED" >&2; exit 1; }
echo "rail exec check: $n services, every exec= target shipped"
