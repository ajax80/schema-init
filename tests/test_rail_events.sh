#!/bin/sh
# Every rail event the source can emit must be in tests/rail_events.txt, which
# test_rail_state checks against rail.c's table: a new event can't silently go quiet.
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp); trap 'rm -f "$T"' EXIT
{
    grep -hoE 'service_log\([^;]*;' engine.c service.c init.c | grep -oE '"[a-z0-9][a-z0-9_/%-]*' | tr -d '"'
    grep -hoE 'bind_hold\([^,]*, *"[a-z0-9_-]*"' engine.c | grep -oE '"[a-z0-9-]*' | tr -d '"'
    grep -hoE 'snprintf\(ev, sizeof ev, "[^" ]*' engine.c | sed 's/.*"//'
} | sed -E 's/%[a-z]+.*/*/' | grep -v -e '^-' -e '^$' | sort -u > "$T"
if ! diff -u tests/rail_events.txt "$T"; then
    echo "FAIL rail_events: the source's event names changed — update tests/rail_events.txt and rail.c's table"
    exit 1
fi
echo "rail_events: ok"
