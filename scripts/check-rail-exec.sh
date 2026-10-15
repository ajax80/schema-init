#!/bin/sh
set -eu
svcd=$1 ks=$2 payload=$3
rel=${RELEASEVER:-44}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

: > "$tmp/rpmfiles"
for r in "$payload"/rpms/*.rpm; do
    rpm -qlp "$r" >> "$tmp/rpmfiles" || { echo "rail exec check: cannot read $r" >&2; exit 1; }
done
[ -s "$tmp/rpmfiles" ] || { echo "rail exec check: no RPMs in $payload/rpms" >&2; exit 1; }

sed -n 's|^rm -f /etc/schema-init/services/\([^ ]*\).*|\1|p' "$ks" > "$tmp/dropped"
awk '{ sub(/[[:space:]]#.*/, "") }
     $1 == "install" && $2 !~ /^-[a-zA-Z]*d/ {
        src = ""
        for (i = 2; i < NF; i++) if ($i ~ /^"\$SRC\//) src = $i
        if (src == "") next
        gsub(/"/, "", src); sub(/^\$SRC\//, "", src)
        dest = $NF
        if (dest ~ /\/$/) { n = split(src, a, "/"); dest = dest a[n] }
        print dest "\t" src
     }' "$ks" > "$tmp/ksinstall"

: > "$tmp/targets"
for f in "$svcd"/*.svc; do
    b=${f##*/}
    skip=
    while read -r pat; do
        case "$b" in $pat) skip=1 ;; esac
    done < "$tmp/dropped"
    [ -n "$skip" ] && continue
    t=$(sed -n 's/^exec=//p' "$f" | tr -d '\r' | tail -1)
    [ -n "$t" ] || continue
    printf '%s\t%s\n' "$b" "$t" >> "$tmp/targets"
    case "$t" in
    */schema-subreaper)
        a=$(sed -n 's/^args=//p' "$f" | tr -d '\r' | head -1)
        case "$a" in /*) printf '%s\t%s\n' "$b" "$a" >> "$tmp/targets" ;; esac ;;
    esac
done

: > "$tmp/ask"
while IFS="$(printf '\t')" read -r b t; do
    grep -qxF "$t" "$tmp/rpmfiles" && continue
    s=$(awk -F '\t' -v t="$t" '$1 == t { print $2; exit }' "$tmp/ksinstall")
    [ -n "$s" ] && [ -e "$payload/$s" ] && continue
    printf '%s\t%s\t%s\n' "$b" "$t" "$(printf '%s' "$t" | sed 's|^/s\{0,1\}bin/|/usr/bin/|; s|^/usr/sbin/|/usr/bin/|')" >> "$tmp/ask"
done < "$tmp/targets"

: > "$tmp/provided"
if [ -s "$tmp/ask" ]; then
    paths=$(cut -f3 "$tmp/ask" | sort -u | paste -sd, -)
    dnf -q repoquery --releasever="$rel" --repo=fedora --repo=updates \
        --setopt=optional_metadata_types=filelists --qf '%{files}\n' --whatprovides "$paths" \
        > "$tmp/provided" 2> "$tmp/dnferr" || {
        echo "rail exec check: dnf repoquery failed:" >&2; cat "$tmp/dnferr" >&2; exit 1; }
fi

bad=0
while IFS="$(printf '\t')" read -r b t u; do
    grep -qxF "$u" "$tmp/provided" && continue
    echo "  $b: $t — no payload RPM, kickstart install or Fedora $rel package ships it" >&2
    bad=1
done < "$tmp/ask"
[ "$bad" = 0 ] || { echo "rail exec check FAILED" >&2; exit 1; }
echo "rail exec check: $(wc -l < "$tmp/targets") exec targets, all shipped"
