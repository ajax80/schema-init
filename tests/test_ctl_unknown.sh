#!/bin/sh
# schema-ctl top runs schema-top (no socket, no root); an unknown command is
# named as unknown, and a root-only socket's hint doesn't vouch for the command.
set -u
cd "$(dirname "$0")/.."
CTL=${1:-build/test/schema-ctl-testhooks}
case $CTL in /*) ;; *) CTL=$PWD/$CTL ;; esac
[ -x "$CTL" ] || { echo "FAIL ctl_unknown: no $CTL"; exit 1; }
T=$(mktemp -d)
trap 'kill $SP 2>/dev/null; rm -rf "$T"' EXIT
SP=
fail() { echo "FAIL ctl_unknown: $*"; sed 's/^/  | /' "$T/out" 2>/dev/null; exit 1; }
export SCHEMA_VERIFY_CTL_SOCK="$T/ctl.sock"

printf '#!/bin/sh\necho "schema-top ran: $*"\n' > "$T/schema-top"
chmod 755 "$T/schema-top"
PATH="$T:$PATH" "$CTL" top -x > "$T/out" 2>&1
rc=$?
[ "$rc" -eq 0 ] || fail "top: exit $rc"
grep -qx 'schema-top ran: -x' "$T/out" || fail "top did not exec schema-top with its args"

if ! command -v python3 >/dev/null 2>&1; then
    echo "ctl_unknown: no python3 — the socket cases are skipped"
    exit 0
fi
# one-shot PID 1 stand-in that answers like engine.c's fallthrough
python3 -c '
import socket, sys
s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(1)
open(sys.argv[1] + ".ready", "w").close()
c, _ = s.accept(); line = c.recv(256).decode().strip()
c.sendall(("err: unknown: %s\n.\n" % line).encode()); c.close()
' "$T/ctl.sock" &
SP=$!
i=0
while [ ! -e "$T/ctl.sock.ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
"$CTL" frobnicate > "$T/out" 2>&1
rc=$?
[ "$rc" -eq 1 ] || fail "unknown command: exit $rc, want 1"
grep -q "unknown command 'frobnicate'" "$T/out" || fail "unknown command not named as such"
grep -q -- '--help' "$T/out" || fail "unknown command does not point at --help"
grep -q '^err: unknown' "$T/out" && fail "raw PID 1 reply leaked"
wait $SP; SP=

if [ "$(id -u)" -ne 0 ]; then
    rm -f "$T/ctl.sock"
    python3 -c 'import socket,sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$T/ctl.sock"
    chmod 000 "$T/ctl.sock"
    "$CTL" frobnicate > "$T/out" 2>&1
    grep -q 'try sudo schema-ctl frobnicate' "$T/out" && fail "root-only hint vouches for an unknown command"
    grep -q 'sudo' "$T/out" || fail "root-only socket gives no sudo hint"
fi
echo "ctl_unknown: ok"
