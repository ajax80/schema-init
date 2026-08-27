#!/bin/sh
# schema-doctor ACL heal, on a real device node under a real session tree.
# Strips the uaccess ACL off a card node, runs schema-doctor --heal in a
# DOCTOR_ROOT sandbox, asserts the active uid regains rw.
set -eu
REPO="${REPO:-$HOME/projects/schema-init}"
DOC="$REPO/scripts/schema-doctor.py"
W="$(mktemp -d /var/tmp/schema-doctor.XXXX)"
trap 'rm -rf "$W"' EXIT
UID_T="$(id -u)"

mkdir -p "$W/run/systemd/sessions" "$W/dev/dri"
printf 'UID=%s\nVTNR=1\n' "$UID_T" > "$W/run/systemd/sessions/1"
: > "$W/dev/dri/card0"
setfacl -b "$W/dev/dri/card0"

DOCTOR_ROOT="$W" python3 "$DOC" --heal >/dev/null
if getfacl -pn "$W/dev/dri/card0" | grep -q "user:$UID_T:rw"; then
  echo ">> RESULT: PASS  (doctor restored uaccess rw)"
else
  echo ">> RESULT: FAIL  (ACL not applied; workdir $W)"; trap - EXIT; exit 1
fi
