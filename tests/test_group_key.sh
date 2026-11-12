#!/bin/sh
# group= needs setgid, so the compiled test runs as uid 0 of a user namespace.
# The binary is built by the Makefile right before this script runs.
set -eu
BIN=/tmp/schema-test-groupkey
[ -x "$BIN" ] || { echo "SKIP: group= (binary not built)"; exit 0; }
if [ "$(id -u)" = 0 ]; then
    exec "$BIN"
fi
unshare -r --map-auto true >/dev/null 2>&1 \
    || { echo "SKIP: group= (no unprivileged user namespace with subordinate uids)"; exit 0; }
exec unshare -r --map-auto "$BIN"
