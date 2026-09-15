#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make schema-systemctl >/dev/null
BIN=$PWD/schema-systemctl
SB=$(mktemp -d)
export SCHEMA_STATE_DIR=$SB/state SCHEMA_SVC_DIR=$SB/svc SCHEMA_UNIT_DIR=$SB/units
mkdir -p "$SCHEMA_STATE_DIR" "$SCHEMA_SVC_DIR" "$SCHEMA_UNIT_DIR"
: > "$SCHEMA_UNIT_DIR/foo.service"

"$BIN" enable foo.service
grep -q foo.service "$SCHEMA_STATE_DIR/pending.list" || { echo "FAIL: not queued"; exit 1; }
rc=0; "$BIN" is-enabled foo || rc=$?; [ "$rc" -eq 0 ] || { echo "FAIL is-enabled"; exit 1; }
rc=0; "$BIN" is-active foo || rc=$?; [ "$rc" -eq 3 ] || { echo "FAIL is-active rc=$rc"; exit 1; }
"$BIN" frobnicate foo; [ $? -eq 0 ] || { echo "FAIL unknown-verb"; exit 1; }
"$BIN" disable foo
rc=0; "$BIN" is-enabled foo || rc=$?; [ "$rc" -eq 1 ] || { echo "FAIL disable rc=$rc"; exit 1; }
rm -rf "$SB"
echo "verify_systemctl_shim: PASS"
