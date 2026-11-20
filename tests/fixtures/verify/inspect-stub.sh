#!/bin/sh
# Stands in for root_inspect's mount+chroot child (SCHEMA_VERIFY_INSPECT, tests only):
# $1 entry id, $2 its expanded options. Records to stderr, --check output to stdout.
# Per-entry overrides in $VFX: init-<id>, ctl-<id> (extra records), rc-<id>, check-<id>.out.
id=$1
init=$(printf "%s\n" "$2" | tr " " "\n" | sed -n "s/^init=//p" | tail -n 1)
[ -f "$VFX/init-$id" ] && init=$(cat "$VFX/init-$id")
printf "I %s\n" "${init:-/sbin/init}" >&2
case $init in *schema-init*) ;; *) exit 0 ;; esac
printf "S\n" >&2
[ -f "$VFX/ctl-$id" ] && cat "$VFX/ctl-$id" >&2
printf "B %s\nC %s\n" "$(cat "$VFX/bootok" 2>/dev/null || echo 1)" "$(cat "$VFX/rc-$id" 2>/dev/null || echo 0)" >&2
f=$VFX/check-$id.out
[ -f "$f" ] || f=$VFX/check.out
cat "$f"
