#!/bin/sh
# T11: prove the real schema-logind.py and schema-systemd1.py register on the
# schema-dbus C broker (SP1) and answer a representative call — the shims are
# unmodified; only DBUS_SYSTEM_BUS_ADDRESS points them at the broker's socket.
#
# The shims own org.freedesktop.{login1,systemd1}, which the dissolved system.conf
# gates on context=user:root. So the broker must see the shims' peer creds as uid
# 0. We get that WITHOUT real root — and without touching the live box — by running
# the whole thing inside a `unshare --mount --map-root-user` user namespace:
#   - our uid maps to 0 inside, so SO_PEERCRED between broker and shim reads 0;
#   - tmpfs over /run/systemd and /run/user contains every write the shims make
#     (schema-systemd1.py unlinks+rebinds /run/systemd/private, the live socket
#     root systemctl uses — that MUST NOT reach the real /run).
# The policy is a small self-contained fixture (root-context own grants), not the
# private corpus — hermetic and committable.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# --- re-exec into a private mount + user namespace where we are uid 0 ----------
if [ "$(id -u)" != "0" ] || [ -z "$SDBUS_SHIM_NS" ]; then
    exec env SDBUS_SHIM_NS=1 unshare --mount --map-root-user "$0" "$@"
fi

WORK="$(mktemp -d)"
SOCK="$WORK/bus.sock"
ADDR="unix:path=$SOCK"
POL="$WORK/policy.txt"
CG="$WORK/cgroup"
mkdir -p "$CG"

# tmpfs over the paths the shims write, so nothing leaks to the live /run.
mount -t tmpfs none /run/systemd
mount -t tmpfs none /run/user

BPID="" ; LPID="" ; SPID=""
cleanup() {
    for p in "$LPID" "$SPID" "$BPID"; do [ -n "$p" ] && kill "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT

# --- minimal hermetic policy: root may own the well-known names the shims claim -
cat > "$POL" <<'EOF'
context=default
deny=own:*
allow=send_destination:*
allow=receive_sender:*
context=user:root
allow=own:org.freedesktop.login1
allow=own:org.freedesktop.systemd1
allow=own:org.freedesktop.hostname1
allow=own:org.freedesktop.locale1
allow=own:org.freedesktop.timedate1
allow=own:org.freedesktop.ConsoleKit
EOF

# --- start the broker ---------------------------------------------------------
SCHEMA_DBUS_SOCKET="$SOCK" SCHEMA_DBUS_POLICY="$POL" \
    "$ROOT/schema-dbus" --system >"$WORK/broker.log" 2>&1 &
BPID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { echo "FAIL: broker did not bind $SOCK"; cat "$WORK/broker.log"; exit 1; }

# --- start the two real shims, pointed at the broker --------------------------
common_env="DBUS_SYSTEM_BUS_ADDRESS=$ADDR SCHEMA_CGROUP_ROOT=$CG"
env $common_env python3 "$ROOT/scripts/schema-logind.py"   >"$WORK/logind.log"   2>&1 &
LPID=$!
env $common_env python3 "$ROOT/scripts/schema-systemd1.py" >"$WORK/systemd1.log" 2>&1 &
SPID=$!

# --- wait for both well-known names to be owned -------------------------------
want_login1="org.freedesktop.login1"
want_systemd1="org.freedesktop.systemd1"
owned=""
for _ in $(seq 1 40); do
    owned="$(busctl --address="$ADDR" list --acquired --no-legend 2>/dev/null | awk '{print $1}')"
    if echo "$owned" | grep -qx "$want_login1" && echo "$owned" | grep -qx "$want_systemd1"; then
        break
    fi
    kill -0 "$LPID" 2>/dev/null || { echo "FAIL: schema-logind.py exited early"; cat "$WORK/logind.log"; exit 1; }
    kill -0 "$SPID" 2>/dev/null || { echo "FAIL: schema-systemd1.py exited early"; cat "$WORK/systemd1.log"; exit 1; }
    sleep 0.25
done

echo "1. both shims own their well-known names on the C broker"
echo "$owned" | grep -qx "$want_login1"   || { echo "FAIL: $want_login1 not owned"; cat "$WORK/logind.log"; exit 1; }
echo "$owned" | grep -qx "$want_systemd1" || { echo "FAIL: $want_systemd1 not owned"; cat "$WORK/systemd1.log"; exit 1; }

echo "2. login1 answers a representative call (Manager.ListSeats)"
busctl --address="$ADDR" call "$want_login1" /org/freedesktop/login1 \
    org.freedesktop.login1.Manager ListSeats >/dev/null 2>"$WORK/e1" \
    || { echo "FAIL: ListSeats errored"; cat "$WORK/e1" "$WORK/logind.log"; exit 1; }

echo "3. systemd1 answers a representative call (Manager.ListUnits)"
busctl --address="$ADDR" call "$want_systemd1" /org/freedesktop/systemd1 \
    org.freedesktop.systemd1.Manager ListUnits >/dev/null 2>"$WORK/e2" \
    || { echo "FAIL: ListUnits errored"; cat "$WORK/e2" "$WORK/systemd1.log"; exit 1; }

echo "4. broker still alive"
kill -0 "$BPID" 2>/dev/null || { echo "FAIL: broker died"; cat "$WORK/broker.log"; exit 1; }

echo "sdbus_shim_check: ALL OK"
