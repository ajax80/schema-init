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

BROKER=${SCHEMA_DBUS_BROKER:-$(find_bin schema-dbus)}
[ -e "${SCHEMA_DBUS_GATE:-/etc/schema-init/dbus-broker}" ] || BROKER=""
STOCK=$(find_bin dbus-daemon)

# wait_for_socket <pid> <tries>: 0.2s per try; gives up early if <pid> dies.
# The broker's first start after a cold boot took over 2s (v0.4.18 ISO VM,
# 2026-10-24), so it gets 15s: falling back while it is still coming up left
# two buses racing for one path.
wait_for_socket() {
    i=0
    while [ "$i" -lt "$2" ]; do
        [ -S "$XDG_RUNTIME_DIR/bus" ] && return 0
        [ -d /proc/"$1" ] || return 1
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

BROKER_PID=""
STOCK_PID=""

stop_pid() {   # TERM, then KILL after 2s
    kill "$1" 2>/dev/null
    i=0
    while [ -d /proc/"$1" ] && [ "$i" -lt 20 ]; do sleep 0.1; i=$((i + 1)); done
    [ -d /proc/"$1" ] && kill -9 "$1" 2>/dev/null
}

# A previous attempt's bus can still be ALIVE: its reaper (below) only fires
# once that attempt's session pid is gone, polling once a second, and the
# autologin loop respawns faster than that. If it reaps after we bind, a
# stock dbus-daemon unlinks the socket path on exit -- ours -- and every
# client of this session gets ENOENT (plasmashell crash-looped on a black
# screen, v0.4.18 ISO VM, 2026-10-24). So stop it first. Only when its
# session is gone: a second run beside a live session must not kill that
# session's bus. The socket path in argv (dbus-daemon) or environ
# (schema-dbus) guards against a recycled pid, e.g. the at-spi bus.
PIDFILE="$XDG_RUNTIME_DIR/schema-session-bus.pid"
holds_our_socket() {
    tr '\0' '\n' < /proc/"$1"/cmdline 2>/dev/null | grep -qx -- "--address=unix:path=$XDG_RUNTIME_DIR/bus" ||
        tr '\0' '\n' < /proc/"$1"/environ 2>/dev/null | grep -qx "SCHEMA_DBUS_SOCKET=$XDG_RUNTIME_DIR/bus"
}
old_session="" old_bus=""
[ -r "$PIDFILE" ] && read -r old_session old_bus < "$PIDFILE"
if [ -n "$old_bus" ] && ! [ -d /proc/"$old_session" ] && holds_our_socket "$old_bus"; then
    stop_pid "$old_bus"
fi

# A bus that crashed or was killed can also leave its socket file behind
# (schema-dbus does not unlink it on exit), and the next bind() then fails
# with every later connect getting ECONNREFUSED (2026-09-22, SP4 cutover).
rm -f "$XDG_RUNTIME_DIR/bus"

# schema-user's routed activation files come first: schema-import --user
# writes one only for a name whose SystemdService= it runs, built from the
# file that would have won below
if [ -n "$BROKER" ]; then
    SCHEMA_DBUS_SOCKET="$XDG_RUNTIME_DIR/bus" \
    SCHEMA_DBUS_SVCDIRS="$XDG_RUNTIME_DIR/schema-user/dbus-services:$HOME/.local/share/dbus-1/services:/usr/local/lib/schema/dbus-1/services:/usr/share/dbus-1/services" \
    SCHEMA_DBUS_MASKFILE=/dev/null \
    "$BROKER" &
    BROKER_PID=$!
    echo "$$ $BROKER_PID" > "$PIDFILE"
fi

if [ -z "$BROKER" ] || ! wait_for_socket "$BROKER_PID" 75; then
    echo "schema-dbus-session-run: broker unavailable — falling back to stock dbus-daemon" >&2
    # The watcher below must watch the bus that actually serves the session,
    # or a broker that dies here kills a healthy fallback session.
    if [ -n "$BROKER_PID" ]; then
        stop_pid "$BROKER_PID"
        BROKER_PID=""
        rm -f "$XDG_RUNTIME_DIR/bus"
    fi
    if [ -z "$STOCK" ]; then
        echo "schema-dbus-session-run: no dbus-daemon to fall back to — no session bus" >&2
        exit 1
    fi
    # Same activation dirs and order the broker gets: schema-user's routed ones,
    # the user's, then ours, then
    # the stock ones that session.conf adds (first definition wins).
    CONF="$XDG_RUNTIME_DIR/schema-session-bus.conf"
    printf '%s\n' '<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">' \
        '<busconfig>' \
        "  <servicedir>$XDG_RUNTIME_DIR/schema-user/dbus-services</servicedir>" \
        "  <servicedir>$HOME/.local/share/dbus-1/services</servicedir>" \
        '  <servicedir>/usr/local/lib/schema/dbus-1/services</servicedir>' \
        '  <include>/usr/share/dbus-1/session.conf</include>' \
        '</busconfig>' > "$CONF"
    "$STOCK" --config-file="$CONF" --address="unix:path=$XDG_RUNTIME_DIR/bus" --nofork &
    STOCK_PID=$!
    echo "$$ $STOCK_PID" > "$PIDFILE"
    wait_for_socket "$STOCK_PID" 25 || echo "schema-dbus-session-run: socket still not up, continuing anyway" >&2
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
