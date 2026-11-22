#!/bin/sh
# prune keeps the newest KEEP snapshots and never deletes the one grubenv's
# schema_fallback_id names; delete refuses it.
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
rc=0
fail() { echo "FAIL: $*"; rc=1; }
mkdir "$T/bin" "$T/top"
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$T/bin/$1"; chmod 755 "$T/bin/$1"; }
stub id 'echo 0'
stub findmnt 'case "$*" in *FSTYPE*) echo btrfs ;; *SOURCE*) echo /dev/fake ;; *) exit 1 ;; esac'
stub mktemp 'echo '"$T"'/top'
stub mount 'exit 0'
stub umount 'exit 0'
stub rmdir 'exit 0'
# rm is stubbed so prune can never touch the real /boot
stub rm 'echo "$*" >> '"$T"'/rm'
stub btrfs 'echo "$*" >> '"$T"'/btrfs'
stub grub2-editenv 'case "$*" in *list*) cat '"$T"'/grubenv 2>/dev/null ;; esac'

S=schema-snap
setup() {
    rm -rf "$T/top"/* "$T/btrfs" "$T/rm"
    for d in 20261101-000000-a 20261102-000000-b 20261103-000000-c 20261104-000000-d 20261105-000000-e; do
        mkdir "$T/top/$S-$d" "$T/top/$S-$d-home"
    done
}
prune() { PATH=$T/bin:$PATH SCHEMA_SNAP_KEEP=3 bash scripts/schema-snapshot prune > "$T/out" 2>&1; }
deleted() { grep -q "subvolume delete $T/top/$S-$1\$" "$T/btrfs" 2>/dev/null; }

# unarmed: the two oldest go
setup; rm -f "$T/grubenv"; prune || fail "unarmed: exit $?: $(cat "$T/out")"
deleted 20261101-000000-a || fail "unarmed: oldest kept"
deleted 20261102-000000-b || fail "unarmed: second oldest kept"
deleted 20261103-000000-c && fail "unarmed: pruned past KEEP"

# armed against the oldest: it survives on top of the KEEP newest
setup; printf 'boot_counter=2\nschema_fallback_id=%s-20261101-000000-a\n' "$S" > "$T/grubenv"; prune || fail "armed: exit $?: $(cat "$T/out")"
deleted 20261101-000000-a && fail "armed: fallback snapshot pruned"
deleted 20261101-000000-a-home && fail "armed: fallback home pruned"
grep -q "$S-20261101-000000-a" "$T/rm" 2>/dev/null && fail "armed: fallback boot entry removed"
deleted 20261102-000000-b || fail "armed: b kept"
deleted 20261103-000000-c && fail "armed: pruned past KEEP newest"

# armed against the newest (what arm does): it counts toward KEEP, no extra kept
setup; printf 'schema_fallback_id=%s-20261105-000000-e\n' "$S" > "$T/grubenv"; prune || fail "newest: exit $?: $(cat "$T/out")"
deleted 20261101-000000-a || fail "newest: a kept"
deleted 20261102-000000-b || fail "newest: b kept"
deleted 20261103-000000-c && fail "newest: pruned past KEEP"
deleted 20261105-000000-e && fail "newest: fallback pruned"

# a fallback id that is a prefix of another snapshot protects only itself
setup; printf 'schema_fallback_id=%s-20261101-000000\n' "$S" > "$T/grubenv"; prune || fail "prefix: exit $?: $(cat "$T/out")"
deleted 20261101-000000-a || fail "prefix: partial id protected a"
deleted 20261102-000000-b || fail "prefix: b kept"
deleted 20261103-000000-c && fail "prefix: pruned past KEEP"

# delete refuses the armed fallback and touches nothing; any other snapshot goes
del() { PATH=$T/bin:$PATH bash scripts/schema-snapshot delete "$1" > "$T/out" 2>&1; }
setup; printf 'schema_fallback_id=%s-20261103-000000-c\n' "$S" > "$T/grubenv"
del "$S-20261103-000000-c" && fail "delete fallback: exited 0"
grep -q 'disarm' "$T/out" || fail "delete fallback: no disarm hint: $(cat "$T/out")"
[ ! -e "$T/btrfs" ] && [ ! -e "$T/rm" ] || fail "delete fallback: something was removed"
del "$S-20261102-000000-b" || fail "delete other: exit $?: $(cat "$T/out")"
deleted 20261102-000000-b || fail "delete other: b kept"

[ $rc = 0 ] && echo "snapshot_prune: ok"
exit $rc
