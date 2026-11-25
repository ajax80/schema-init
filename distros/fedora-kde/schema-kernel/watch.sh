#!/bin/bash
# Rebuild kernel-cachyos-schema in COPR whenever bieszczaders/kernel-cachyos
# ships a new kernel-cachyos, so blakbox gets it from a plain `dnf upgrade`.
# Runs from schema-kernel-watch.svc; phones Jonathan (ntfy) when a release
# needs a hand: mksrpm.sh no longer applies, or the COPR build failed.
#
#   watch.sh [--dry-run]   --dry-run: build the SRPM, but submit and phone nothing
#
# Never retries a version on its own: a failed one is reported once and left
# for a person, because a rebuilt broken patch would just fail again.
set -euo pipefail

UPSTREAM_REPO=copr:copr.fedorainfracloud.org:bieszczaders:kernel-cachyos
COPR_PROJECT="${SCHEMA_KERNEL_COPR:-ajax80/schema-kernel}"
NTFY="${SCHEMA_KERNEL_NTFY:-https://ntfy.sh/blakfleet-watch-imbogcv6oe}"
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/schema-kernel-watch"
MKSRPM="$(dirname "$(realpath "$0")")/mksrpm.sh"
DRY=0
[ "${1:-}" = --dry-run ] && DRY=1

phone() {
    # $1 = priority, $2 = message
    echo "watch: $2"
    [ $DRY = 1 ] && return
    curl -fsS -m 20 -H "Title: schema kernel" -H "Priority: $1" -H "Tags: penguin" \
         --data-binary "$2" "$NTFY" >/dev/null
}

# once per version, so a daily timer doesn't phone every day about the same release
phone_once() {
    # $1 = key, $2 = priority, $3 = message
    [ -e "$STATE/$1" ] && { echo "watch: already reported: $3"; return; }
    phone "$2" "$3"
    [ $DRY = 1 ] || { mkdir -p "$STATE"; touch "$STATE/$1"; }
}

trap 'phone high "watcher crashed at line $LINENO; see schema-ctl log schema-kernel-watch"' ERR

# excludepkgs= because blakbox's dnf.conf excludes stock kernel-cachyos
up_nvr="$(dnf repoquery -q --refresh --repo="$UPSTREAM_REPO" --setopt=excludepkgs= \
          --latest-limit=1 --qf '%{version}-%{release}\n' kernel-cachyos)"
[ -n "$up_nvr" ] || { phone high "can't see kernel-cachyos in $UPSTREAM_REPO"; exit 1; }
# 7.3.6-1.cachyos1.fc44 -> 7.3.6-1.cachyos1.schema (what mksrpm.sh's Release makes)
want="${up_nvr%.fc*}.schema"
echo "watch: upstream kernel-cachyos $up_nvr -> want kernel-cachyos-schema $want"

states="$(copr-cli list-packages "$COPR_PROJECT" --with-all-builds --output-format json |
          WANT="$want" python3 -c '
import json, os, sys
for p in json.load(sys.stdin):
    for b in p["builds"]:
        sp = b.get("source_package") or {}
        if sp.get("version") == os.environ["WANT"]:
            print(b["id"], b["state"])')"

if [ -n "$states" ]; then
    while read -r b; do echo "watch: copr build $b"; done <<<"$states"
    if grep -qvE ' (failed|canceled)$' <<<"$states"; then
        exit 0   # built, or still building
    fi
    id="${states%% *}"
    phone_once "copr-$want" high \
        "COPR build of kernel-cachyos-schema $want failed: https://copr.fedorainfracloud.org/coprs/build/$id/ (blakbox stays on the kernel it has)"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
(cd "$WORK" && dnf download -q --source --repo="$UPSTREAM_REPO" --setopt=excludepkgs= \
               "kernel-cachyos-$up_nvr")
if ! srpm="$("$MKSRPM" "$WORK"/kernel-cachyos-*.src.rpm "$WORK/out" 2>"$WORK/err" | tail -1)"; then
    cat "$WORK/err" >&2
    phone_once "mksrpm-$want" high \
        "mksrpm.sh no longer applies to kernel-cachyos $up_nvr: $(tail -1 "$WORK/err")"
    exit 1
fi

if [ $DRY = 1 ]; then
    echo "watch: dry run: would submit $srpm to $COPR_PROJECT"
    exit 0
fi
out="$(copr-cli build --nowait "$COPR_PROJECT" "$srpm")"
echo "$out"
id="$(echo "$out" | sed -n 's/^Created builds: *//p')"
phone default "building kernel-cachyos-schema $want in COPR: https://copr.fedorainfracloud.org/coprs/build/$id/"
