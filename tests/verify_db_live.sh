#!/bin/sh
# E1 artifact-parity gate: schema-udev's /run/schema-udev/data reproduces real
# /run/udev/data (E: keys) for in-scope subsystems. Wraps the read-only udev-parity
# harness as a pass/fail gate. Writes nothing.
set -e
cd "$(dirname "$0")/.."
make -s parity

out=$(sudo ./udev-parity 2>&1)

miss=$(printf '%s\n' "$out" | sed -n 's/^IN-SCOPE MISSING (db): *//p' | tail -1)
mism=$(printf '%s\n' "$out" | sed -n 's/^VALUE MISMATCHES (db): *//p' | tail -1)

echo "db in-scope missing: ${miss:-?}   db value mismatches: ${mism:-?}"

if [ -z "$miss" ] || [ -z "$mism" ]; then
    echo ">> RESULT: FAIL (could not parse udev-parity db summary)"; exit 1
fi
if [ "$miss" != 0 ] || [ "$mism" != 0 ]; then
    echo ">> RESULT: FAIL (db parity gap — see udev-parity output)"; exit 1
fi
echo ">> RESULT: PASS (schema db reproduces real /run/udev/data E: keys, in-scope)"
