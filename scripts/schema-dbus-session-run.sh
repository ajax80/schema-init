#!/bin/sh
# schema-dbus-session-run.sh <command> [args...]
#
# Drop-in replacement for /usr/libexec/plasma-dbus-run-session-if-needed at
# its call site in schema-plasma-autologin.sh:104. Starts the session bus
# (the schema-dbus C broker, falling back to stock dbus-daemon on failure),
# exports DBUS_SESSION_BUS_ADDRESS, then execs the given command as a child
# -- same shape as the tool it replaces, so the caller's exit-code/lifetime
# tracking (schema-plasma-autologin.sh's `RC=$?` loop) is unaffected.
#
# No --system flag is passed to schema-dbus: that absence is what puts the
# broker in session mode (see schema-dbus.c's g_system_bus). No
# SCHEMA_DBUS_POLICY is set either: the broker's no-policy-file default
# (SDBUS_NO_POLICY_FILE_DEFAULT) already matches session.conf's allow-all
# stance -- see the SP4 design doc, Decision 2.
set -u

if [ $# -lt 1 ]; then
    echo "usage: schema-dbus-session-run.sh <command> [args...]" >&2
    exit 1
fi

XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"

self_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

find_bin() {
    for p in /usr/local/bin/"$1" /usr/bin/"$1" /sbin/"$1" \
             "$self_dir/../$1" "$self_dir/$1"; do
        [ -x "$p" ] && { echo "$p"; return 0; }
    done
    command -v "$1" 2>/dev/null
}

BROKER=$(find_bin schema-dbus)
STOCK=$(find_bin dbus-daemon)

wait_for_socket() {   # up to 2s; a local fork+bind is normally near-instant
    i=0
    while [ "$i" -lt 10 ]; do
        [ -S "$XDG_RUNTIME_DIR/bus" ] && return 0
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

if [ -n "$BROKER" ]; then
    SCHEMA_DBUS_SOCKET="$XDG_RUNTIME_DIR/bus" \
    SCHEMA_DBUS_SVCDIRS="$HOME/.local/share/dbus-1/services:/usr/share/dbus-1/services" \
    SCHEMA_DBUS_MASKFILE=/dev/null \
    "$BROKER" &
fi

if [ -z "$BROKER" ] || ! wait_for_socket; then
    echo "schema-dbus-session-run: broker unavailable — falling back to stock dbus-daemon" >&2
    if [ -z "$STOCK" ]; then
        echo "schema-dbus-session-run: no dbus-daemon to fall back to — no session bus" >&2
        exit 1
    fi
    "$STOCK" --session --address="unix:path=$XDG_RUNTIME_DIR/bus" --nofork &
    wait_for_socket || echo "schema-dbus-session-run: socket still not up, continuing anyway" >&2
fi

exec "$@"
