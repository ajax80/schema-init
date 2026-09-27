#!/bin/sh
# schema-dbus-flip.sh {check|arm|confirm|rollback|is-authoritative|state}
#
# The schema-dbus half of the wizard flip, invoked as root through
# schema-flip-apply (dbus-* subcommands). One flip, one switch: the gate file
# /etc/schema-init/dbus-broker turns the broker on for BOTH buses, and
# dbus.svc is pointed at the system launcher beside this script. Rollback
# restores the stock dbus.svc and removes the gate.
#
# State is separate from the udev flip's (dbus-flip.state / its own boot
# counter) so the two seatbelts never judge or roll back each other.
#
# SCHEMA_DBUS_FLIP_ROOT prefixes every path this script WRITES, for tests.
# schema-flip-apply unsets it; the preflight always uses the real broker and
# the real busconfig.
set -u

R="${SCHEMA_DBUS_FLIP_ROOT:-}"
LIB="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
RUN="$LIB/schema-dbus-run.sh"
DISSECT="$LIB/dissect_policy.py"
SVC="$R/etc/schema-init/services/dbus.svc"
STOCK="$R/var/lib/schema-init/dbus.svc.stock"
GATE="$R/etc/schema-init/dbus-broker"
STATE="$R/var/lib/schema-init/dbus-flip.state"
BOOTS="$R/var/lib/schema-init/dbus-flip-armed-boots"
MASK=/etc/schema-dbus/masked

broker() {
    for p in /usr/local/bin/schema-dbus /usr/bin/schema-dbus; do
        [ -x "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

say() { echo "schema-dbus-flip: $*" >&2; }

# Dissolve THIS box's busconfig and start the broker on a scratch socket with
# it. SCHEMA_DBUS_SOCKET is mandatory: without it the broker unlinks and
# rebinds the live system bus socket.
check() {
    b=$(broker) || { say "schema-dbus binary not installed"; return 1; }
    [ -x "$RUN" ] || { say "launcher missing: $RUN"; return 1; }
    [ -f "$DISSECT" ] || { say "dissolver missing: $DISSECT"; return 1; }
    [ -f "$SVC" ] || { say "no dbus.svc at $SVC"; return 1; }
    t=$(mktemp -d) || return 1
    rc=1
    if ! python3 "$DISSECT" /usr/share/dbus-1/system.conf > "$t/pol" 2> "$t/err" || [ ! -s "$t/pol" ]; then
        say "busconfig dissolve failed: $(head -1 "$t/err")"
    else
        SCHEMA_DBUS_SOCKET="$t/sock" SCHEMA_DBUS_POLICY="$t/pol" SCHEMA_DBUS_MASKFILE="$MASK" \
            "$b" --system > "$t/log" 2>&1 &
        pid=$!
        i=0
        while [ $i -lt 25 ] && [ ! -S "$t/sock" ]; do sleep 0.2; i=$((i + 1)); done
        if dbus-send --bus="unix:path=$t/sock" --print-reply --dest=org.freedesktop.DBus \
                / org.freedesktop.DBus.GetId > /dev/null 2>&1; then
            rc=0
        else
            say "broker did not answer on a scratch socket: $(tail -1 "$t/log")"
        fi
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    fi
    rm -rf "$t"
    return $rc
}

is_broker_svc() { grep -qx "exec=$RUN" "$SVC" 2>/dev/null; }

arm() {
    check || return 1
    install -d "$(dirname "$STOCK")" "$(dirname "$GATE")"
    if ! is_broker_svc; then
        cp -a "$SVC" "$STOCK" || return 1
    fi
    [ -f "$STOCK" ] || { say "no stock dbus.svc backup to roll back to"; return 1; }
    { grep -v -e '^exec=' -e '^args=' -e '^ready_path=' "$STOCK"
      echo "exec=$RUN"
      echo "ready_path=/run/dbus/system_bus_socket"
    } > "$SVC.new" && mv -f "$SVC.new" "$SVC" || return 1
    : > "$GATE"
    echo armed > "$STATE"
    rm -f "$BOOTS"
}

rollback() {
    if [ -f "$STOCK" ]; then
        cp -a "$STOCK" "$SVC" || return 1
    elif is_broker_svc; then
        say "dbus.svc is the broker form and no stock backup exists"
        return 1
    fi
    rm -f "$GATE"
    install -d "$(dirname "$STATE")"
    echo skipped > "$STATE"
    rm -f "$BOOTS"
}

case "${1:-}" in
    check) check ;;
    arm) arm ;;
    confirm)
        install -d "$(dirname "$STATE")"
        echo done > "$STATE"
        rm -f "$BOOTS"
        ;;
    rollback) rollback ;;
    is-authoritative)
        [ -e "$GATE" ] && is_broker_svc || exit 1
        pgrep -f '/schema-dbus --system$' > /dev/null || exit 1
        dbus-send --system --print-reply --dest=org.freedesktop.DBus \
            / org.freedesktop.DBus.GetId > /dev/null 2>&1
        ;;
    state) cat "$STATE" 2>/dev/null || echo unknown ;;
    *)
        echo "usage: schema-dbus-flip.sh {check|arm|confirm|rollback|is-authoritative|state}" >&2
        exit 2
        ;;
esac
