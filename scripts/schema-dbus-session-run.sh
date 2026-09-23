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

BROKER_PID=""
STOCK_PID=""

# A broker from a prior attempt this boot that crashed or was killed can
# leave its socket file behind (nothing here or in schema-dbus itself
# unlinks it on exit). The next attempt's bind() then fails silently -- no
# error text, just no "listening" line and every later connect getting
# ECONNREFUSED -- which orphans the whole session exactly like a dead
# broker does, except now the NEW attempt never had a broker at all. Hit
# live on the second respawn of the third SP4-cutover reboot (2026-09-22):
# attempt 1's session crashed fast (kwin exit rc=1), attempt 2's broker
# then failed to bind on the leftover socket. Safe to remove unconditionally
# here -- by this point any previous broker's reaper watchdog (below) has
# either already unlinked it or the process holding it is already gone,
# since this script only runs once per fresh session attempt.
rm -f "$XDG_RUNTIME_DIR/bus"

if [ -n "$BROKER" ]; then
    SCHEMA_DBUS_SOCKET="$XDG_RUNTIME_DIR/bus" \
    SCHEMA_DBUS_SVCDIRS="$HOME/.local/share/dbus-1/services:/usr/share/dbus-1/services" \
    SCHEMA_DBUS_MASKFILE=/dev/null \
    "$BROKER" &
    BROKER_PID=$!
fi

if [ -z "$BROKER" ] || ! wait_for_socket; then
    echo "schema-dbus-session-run: broker unavailable — falling back to stock dbus-daemon" >&2
    if [ -z "$STOCK" ]; then
        echo "schema-dbus-session-run: no dbus-daemon to fall back to — no session bus" >&2
        exit 1
    fi
    "$STOCK" --session --address="unix:path=$XDG_RUNTIME_DIR/bus" --nofork &
    STOCK_PID=$!
    wait_for_socket || echo "schema-dbus-session-run: socket still not up, continuing anyway" >&2
fi

# We're about to exec into $@, which keeps this same PID alive as the session
# command (that's the point -- see the header comment). That means a trap set
# here would never fire: exec discards it. So instead, hand a tiny watchdog
# to the background that polls for *this* PID to disappear -- which only
# happens once the exec'd session command itself exits, however it exits
# (clean logout or a crash) -- and only then reaps the broker/fallback. Without
# this, every session crash-restart orphaned a live schema-dbus still holding
# the old $XDG_RUNTIME_DIR/bus socket open (found 2026-09-22, SP4 cutover).
SESSION_PID=$$
if [ -n "$BROKER_PID" ] || [ -n "$STOCK_PID" ]; then
    (
        # $$ is this script's own PID -- preserved across the exec below, so
        # this fires exactly once the session command itself is gone. NOT
        # $PPID: in a subshell $PPID is the *parent* (runuser, uid 0), and
        # `kill -0` on a root pid from this uid-1000 subshell returns EPERM,
        # which the loop below read as "session gone" -> it killed the broker
        # ~1s into every boot. That is the 2026-09-22 SP4 black screen: dead
        # bus, stale socket, every KF6 app aborting on connect. (Crystal)
        session_pid=$SESSION_PID
        while [ -d /proc/"$session_pid" ]; do
            sleep 1
        done
        [ -n "$BROKER_PID" ] && kill "$BROKER_PID" 2>/dev/null
        [ -n "$STOCK_PID" ] && kill "$STOCK_PID" 2>/dev/null
    ) &

    # Nothing previously monitored the broker's own health once the session
    # was up -- if it (or the stock fallback) dies mid-session but kwin stays
    # alive, every session app is left holding a dead bus connection with no
    # automatic recovery (found the hard way on the first reboot of the SP4
    # cutover 2026-09-22: schema-dbus died independently mid-boot, kwin never
    # noticed, and bringing plasmashell back up took hand recovery). Watch
    # the broker's PID here too; if it disappears while the session is still
    # up, kill the session so the autologin loop's normal crash-restart path
    # (re-forks kwin, re-runs this whole script, starts a fresh broker with a
    # correct env) takes over instead of leaving a half-dead session running.
    (
        # Same $$-not-$PPID rule as above, and for a second reason: the
        # recovery `kill` has to land on a pid this uid can signal. Killing
        # $PPID (root runuser) was EPERM -> silent no-op -> a broker death
        # left the session half-alive forever with no respawn. $$ is
        # ajax80-owned, and killing it exits the session command so the
        # autologin loop's crash-restart path takes over. (Crystal)
        session_pid=$SESSION_PID
        watch_pid=${BROKER_PID:-$STOCK_PID}
        while [ -d /proc/"$session_pid" ] && [ -d /proc/"$watch_pid" ]; do
            sleep 2
        done
        [ -d /proc/"$watch_pid" ] || kill "$session_pid" 2>/dev/null
    ) &
fi

exec "$@"
