#!/bin/sh
# Live interop test for schema-dbus: start the broker on a scratch socket, then
# exercise it with busctl (driver methods) and the two-client interop harness
# (client->client routing, reply-tracking, unix-fd passing). Exits non-zero on
# any failure. Run from the repo root after `make schema-dbus`.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOCK="/tmp/sdbus-interop-$$.sock"
export SCHEMA_DBUS_SOCKET="$SOCK"
ADDR="unix:path=$SOCK"

cleanup() { [ -n "$DPID" ] && kill "$DPID" 2>/dev/null; rm -f "$SOCK"; }
trap cleanup EXIT

rm -f "$SOCK"
"$ROOT/schema-dbus" --system >/tmp/sdbus-interop-$$.log 2>&1 &
DPID=$!

# wait for the socket to appear
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { echo "FAIL: broker did not bind $SOCK"; cat /tmp/sdbus-interop-$$.log; exit 1; }

echo "1. busctl list shows the driver"
busctl --address="$ADDR" list 2>/dev/null | grep -q '^org.freedesktop.DBus' \
    || { echo "FAIL: org.freedesktop.DBus not listed"; exit 1; }

echo "2. GetId returns a machine id"
ID=$(busctl --address="$ADDR" call org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus GetId 2>/dev/null)
[ -n "$ID" ] || { echo "FAIL: empty GetId"; exit 1; }

echo "3. two-client routing + reply + unix-fd passing"
cc -O2 -std=c99 -D_GNU_SOURCE -I"$ROOT" $(pkg-config --cflags dbus-1) \
    "$ROOT/tests/sdbus_interop.c" -o "/tmp/sdbus_interop-$$" $(pkg-config --libs dbus-1)
"/tmp/sdbus_interop-$$" || { echo "FAIL: interop harness"; exit 1; }
rm -f "/tmp/sdbus_interop-$$"

echo "4. broker still alive after the run"
kill -0 "$DPID" 2>/dev/null || { echo "FAIL: broker died"; cat /tmp/sdbus-interop-$$.log; exit 1; }

rm -f /tmp/sdbus-interop-$$.log
echo "sdbus_live_interop: ALL OK"
