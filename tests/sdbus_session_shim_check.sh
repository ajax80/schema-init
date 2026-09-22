#!/bin/sh
# Scratch-bus proof for the SP4 session-bus fixes (Fix 1a/1b/2/3/4),
# mirroring tests/sdbus_shim_check.sh's shape but for session mode --
# no root/unshare needed, since the session bus has no privilege boundary
# (the broker runs as whoever invokes this script, exactly like it would
# for a real login).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
SOCK="$WORK/bus.sock"
ADDR="unix:path=$SOCK"
LOCALDIR="$WORK/local-services"
SYSDIR="$WORK/system-services"
mkdir -p "$LOCALDIR" "$SYSDIR"

BPID=""
cleanup() {
    [ -n "$BPID" ] && kill "$BPID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# --- fixture: a real, User=-less .service file that writes a marker file
# recording who ran it -- the direct regression test for Fix 1a/1b (must
# NOT be root, since this whole script runs unprivileged) and Fix 4 dir 1
# specifically (this file only lives in LOCALDIR, mirroring
# ~/.local/share/dbus-1/services' real role -- see the SP4 design doc's
# Fix 4 section on org.freedesktop.systemd1.service).
#
# sdbus__split_argv does naive whitespace splitting (real dbus .service
# Exec= lines are unquoted plain paths + flags, per its own comment) --
# so the exec target here must be a single-token script path, not an
# inline `sh -c "..."` with spaces/quoting.
MARKER="$WORK/activated-as-uid"
WINNER_SCRIPT="$WORK/winner.sh"
cat > "$WINNER_SCRIPT" <<EOF
#!/bin/sh
id -u > $MARKER
sleep 5
EOF
chmod +x "$WINNER_SCRIPT"
cat > "$LOCALDIR/com.example.SmokeTest.service" <<EOF
[D-BUS Service]
Name=com.example.SmokeTest
Exec=$WINNER_SCRIPT
EOF

# --- a same-named entry in SYSDIR that must be shadowed (Fix 4 override
# precedence) -- if this one wins instead, MARKER never gets written
# because this Exec= doesn't touch it.
LOSER_SCRIPT="$WORK/loser.sh"
cat > "$LOSER_SCRIPT" <<'EOF'
#!/bin/sh
sleep 5
EOF
chmod +x "$LOSER_SCRIPT"
cat > "$SYSDIR/com.example.SmokeTest.service" <<EOF
[D-BUS Service]
Name=com.example.SmokeTest
Exec=$LOSER_SCRIPT
EOF

# --- start the broker in session mode: no --system, SVCDIRS set, no
# policy file (Fix 3's default applies) ------------------------------
SCHEMA_DBUS_SOCKET="$SOCK" \
SCHEMA_DBUS_SVCDIRS="$LOCALDIR:$SYSDIR" \
SCHEMA_DBUS_MASKFILE=/dev/null \
    "$ROOT/schema-dbus" >"$WORK/broker.log" 2>&1 &
BPID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { echo "FAIL: broker did not bind $SOCK"; cat "$WORK/broker.log"; exit 1; }

echo "1. RequestName / ListNames work on the session broker"
busctl --address="$ADDR" list --acquired --no-legend >/dev/null 2>"$WORK/e1" \
    || { echo "FAIL: ListNames errored"; cat "$WORK/e1" "$WORK/broker.log"; exit 1; }

echo "2. cold-activate a real, User=-less .service (Fix 1a/1b + Fix 4 dir precedence)"
busctl --address="$ADDR" call com.example.SmokeTest / org.freedesktop.DBus.Peer Ping \
    >/dev/null 2>"$WORK/e2" || true   # activation itself is what we're checking, not this call's own success
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -f "$MARKER" ] && break; sleep 0.2; done
[ -f "$MARKER" ] || { echo "FAIL: activated service never wrote its marker (wrong dir won, or spawn failed)"; cat "$WORK/broker.log"; exit 1; }
GOT_UID="$(cat "$MARKER")"
MY_UID="$(id -u)"
[ "$GOT_UID" = "$MY_UID" ] || { echo "FAIL: activated as uid $GOT_UID, expected $MY_UID (Fix 1a/1b regression)"; exit 1; }

echo "3. broadcast signal delivered to a subscribed match (Fix 3)"
busctl --address="$ADDR" wait / com.example.sig Changed >"$WORK/wait.log" 2>&1 &
WAITPID=$!
sleep 0.3
busctl --address="$ADDR" emit / com.example.sig Changed >/dev/null 2>"$WORK/e3" \
    || { echo "FAIL: emit errored"; cat "$WORK/e3"; kill "$WAITPID" 2>/dev/null; exit 1; }
if wait "$WAITPID"; then
    :
else
    echo "FAIL: busctl wait never saw the broadcast (Fix 3 regression)"; cat "$WORK/wait.log"; exit 1
fi

echo "4. broker still alive throughout"
kill -0 "$BPID" 2>/dev/null || { echo "FAIL: broker died"; cat "$WORK/broker.log"; exit 1; }

echo "sdbus_session_shim_check: ALL OK"
