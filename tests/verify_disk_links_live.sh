#!/bin/sh
# E1 artifact-parity gate: schema-udev's isolated /dev/schema/disk vs real /dev/disk.
# Reads only; asserts the live daemon's disk links match reality before any flip.
# Dangerous directions FAIL (flip would lose/corrupt a link); benign extras INFO.
set -e
cd "$(dirname "$0")/.."

SCHEMA=/dev/schema/disk
REAL=/dev/disk
# In-scope: identity + label + diskseq classes schema-udev derives today.
# Deferred (excluded, documented): by-id (needs *_id builtins), by-path (commit e92ecdd).
INSCOPE="by-uuid by-partuuid by-label by-partlabel by-diskseq"

[ -d "$SCHEMA" ] || { echo "FAIL: $SCHEMA missing — is schema-udev running?"; exit 1; }

devno() { stat -Lc '%t:%T' "$1" 2>/dev/null; }   # -L: follow link to the device node

fail=0; n_fwd=0; n_extra=0; n_rev=0

# FORWARD (only-ours): each schema link that also exists in real must point at the
# same device (name-in-both + different target = corruption = FAIL). A schema link
# with no real counterpart is a benign extra (INFO) — the flip would add a harmless
# link, never lose one.
for s in $INSCOPE; do
    [ -d "$SCHEMA/$s" ] || continue
    for l in "$SCHEMA/$s"/*; do
        [ -e "$l" ] || continue
        name=$(basename "$l"); n_fwd=$((n_fwd + 1))
        if [ -e "$REAL/$s/$name" ]; then
            sd=$(devno "$l"); rd=$(devno "$REAL/$s/$name")
            if [ "$sd" != "$rd" ]; then
                echo "FAIL target: $s/$name schema->$sd real->$rd"; fail=1
            fi
        else
            n_extra=$((n_extra + 1))
            echo "INFO only-ours (benign extra): $s/$name -> $(devno "$l")"
        fi
    done
done
echo "forward: $n_fwd schema links checked, $n_extra benign extras"

# REVERSE (completeness): every in-scope real link MUST have a schema link to the
# same device. A missing one = the flip loses that link = FAIL.
for s in $INSCOPE; do
    [ -d "$REAL/$s" ] || continue
    for l in "$REAL/$s"/*; do
        [ -e "$l" ] || continue
        name=$(basename "$l"); n_rev=$((n_rev + 1))
        if [ ! -e "$SCHEMA/$s/$name" ]; then
            echo "FAIL completeness: real $s/$name has NO schema link"; fail=1
        elif [ "$(devno "$SCHEMA/$s/$name")" != "$(devno "$l")" ]; then
            echo "FAIL completeness-target: $s/$name schema->$(devno "$SCHEMA/$s/$name") real->$(devno "$l")"; fail=1
        fi
    done
done
echo "reverse: $n_rev in-scope real links checked"

[ "$fail" = 0 ] || { echo ">> RESULT: FAIL"; exit 1; }
echo ">> RESULT: PASS (in-scope disk links == real; by-id/by-path deferred; $n_extra benign extras)"
