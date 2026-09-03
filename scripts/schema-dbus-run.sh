#!/bin/sh
# SP1 cutover launcher for the schema-dbus C broker.
#
# The broker needs the system D-Bus policy as a dissolved file (it does NOT parse
# busconfig XML itself); its built-in default denies all name ownership, which
# would break every service. So dissolve the LIVE busconfig into the broker's
# policy format at boot, then exec the broker on the system bus socket. If the
# dissolve fails for any reason, fall back to stock dbus-daemon so the box still
# boots with a working bus — the cutover is self-healing, never a dead bus.
set -u

self_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

find_bin() {
    for p in /usr/local/bin/"$1" /usr/bin/"$1" /sbin/"$1" \
             "$self_dir/../$1" "$self_dir/$1"; do
        [ -x "$p" ] && { echo "$p"; return 0; }
    done
    command -v "$1" 2>/dev/null
}

DISSECT=""
for p in "$self_dir/../lib/schema-init/dissect_policy.py" \
         /usr/local/lib/schema-init/dissect_policy.py \
         /usr/lib/schema-init/dissect_policy.py \
         "$self_dir/../tools/dbus-learn/dissect_policy.py"; do
    [ -f "$p" ] && { DISSECT="$p"; break; }
done

BROKER=$(find_bin schema-dbus)
STOCK=$(find_bin dbus-daemon)
POLDIR=/run/schema-init
POL="$POLDIR/dbus.policy"

fallback() {
    echo "schema-dbus-run: $1 — falling back to stock dbus-daemon" >&2
    exec "$STOCK" --system --nofork
}

[ -n "$BROKER" ] || fallback "schema-dbus binary not found"
[ -n "$DISSECT" ] || fallback "dissect_policy.py not found"

mkdir -p "$POLDIR"
if python3 "$DISSECT" /usr/share/dbus-1/system.conf > "$POL.tmp" 2>/dev/null && [ -s "$POL.tmp" ]; then
    mv -f "$POL.tmp" "$POL"
else
    rm -f "$POL.tmp"
    fallback "policy dissolve failed"
fi

exec env SCHEMA_DBUS_POLICY="$POL" "$BROKER" --system
