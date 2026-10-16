#!/bin/sh
# SP1 cutover launcher for the schema-dbus C broker.
#
# The broker needs the system D-Bus policy as a dissolved file (it does NOT parse
# busconfig XML itself); its built-in default denies all name ownership, which
# would break every service. So dissolve the LIVE busconfig into the broker's
# policy format at boot, then exec the broker on the system bus socket. If the
# dissolve fails for any reason, fall back to stock dbus-daemon so the box still
# boots with a working bus — the cutover is self-healing, never a dead bus.
# The dissolved policy is cached in /var/cache/schema-init and reused until a
# D-Bus config file or the dissolver is newer than the cache (python3 cold-start
# is ~0.6 s on a spinning disk, and every bus client waits on it).
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
for p in "$self_dir/dissect_policy.py" \
         "$self_dir/../lib/schema-init/dissect_policy.py" \
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
    [ -n "$STOCK" ] || { echo "schema-dbus-run: no dbus-daemon to fall back to — no system bus" >&2; exit 1; }
    exec "$STOCK" --system --nofork
}

[ -e "${SCHEMA_DBUS_GATE:-/etc/schema-init/dbus-broker}" ] || fallback "broker not armed"
[ -n "$BROKER" ] || fallback "schema-dbus binary not found"
[ -n "$DISSECT" ] || fallback "dissect_policy.py not found"

CACHE=/var/cache/schema-init/dbus.policy

mkdir -p "$POLDIR"
if [ -s "$CACHE" ] && [ -z "$(find /usr/share/dbus-1/system.conf /usr/share/dbus-1/system.d /etc/dbus-1 "$DISSECT" -newer "$CACHE" 2>/dev/null | head -n1)" ] &&
   cp "$CACHE" "$POL.tmp" 2>/dev/null; then
    mv -f "$POL.tmp" "$POL"
elif python3 "$DISSECT" /usr/share/dbus-1/system.conf > "$POL.tmp" 2>/dev/null && [ -s "$POL.tmp" ]; then
    mv -f "$POL.tmp" "$POL"
    mkdir -p "${CACHE%/*}" && cp "$POL" "$CACHE.tmp" 2>/dev/null && mv -f "$CACHE.tmp" "$CACHE"
else
    rm -f "$POL.tmp"
    fallback "policy dissolve failed"
fi

exec env SCHEMA_DBUS_POLICY="$POL" "$BROKER" --system
