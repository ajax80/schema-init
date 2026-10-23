#!/bin/sh
# schema-bus-activate --user: a unit schema-user knows is started there and
# the name waited for; anything else runs the activation file's Exec.
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }
mkdir "$T/bin"
# schema-ctl stub: knows "known" (state from $T/state), logs every call
cat > "$T/bin/ctl" <<EOF
#!/bin/sh
echo "\$*" >> "$T/log"
[ "\$1" = --user ] || exit 9
[ "\$3" = known ] || { echo "err: not found: \$3"; exit 1; }
[ "\$2" = status ] && echo "  known pid=0 state=\$(cat "$T/state")"
exit 0
EOF
printf '#!/bin/sh\necho "gdbus $*" >> "%s/log"\n' "$T" > "$T/bin/gdbus"
chmod 755 "$T/bin/ctl" "$T/bin/gdbus"
run() { PATH="$T/bin:$PATH" SCHEMA_CTL="$T/bin/ctl" XDG_RUNTIME_DIR=$T sh scripts/schema-bus-activate "$@"; }

echo FULL_TRUST > "$T/state"
run --user known org.x.Known /bin/echo direct > "$T/out" || fail "known: exit $?"
grep -qx -- "--user start known" "$T/log" || fail "known: no start ($(cat "$T/log"))"
grep -qx "gdbus wait --session --timeout 20 org.x.Known" "$T/log" || fail "known: no session wait"
grep -q direct "$T/out" && fail "known: ran the direct Exec too"

: > "$T/log"
echo DORMANT > "$T/state"
run --user known org.x.Known /bin/echo direct > /dev/null && fail "dormant: exit 0"
grep -q gdbus "$T/log" && fail "dormant: waited anyway"

: > "$T/log"
[ "$(run --user other org.x.Other /bin/echo direct)" = direct ] || fail "unknown unit: no direct Exec"
grep -q start "$T/log" && fail "unknown unit: started"

[ "$(PATH="$T/bin:$PATH" SCHEMA_CTL=/nonexistent XDG_RUNTIME_DIR=$T sh scripts/schema-bus-activate --user known org.x.Known /bin/echo direct)" = direct ] \
    || fail "no schema-ctl: no direct Exec"
echo "test_bus_activate: ok"
