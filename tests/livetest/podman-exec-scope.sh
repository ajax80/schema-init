#!/bin/sh
# podman exec from a login shell into a schema-user Quadlet container: the
# session shim honours podman's own podman-<pid>.scope request and the root
# bridge moves it into user@U.service/user.slice/ (spec 2026-11-03).
# Run as the user, from a desktop terminal, on a box with the image
# docker.io/library/debian:trixie already pulled.
U=scopetest C=systemd-scopetest Q=$HOME/.config/containers/systemd/scopetest.container
ID=$(id -u)
SL=/sys/fs/cgroup/user.slice/user-$ID.slice/user@$ID.service/user.slice
LOG=$XDG_RUNTIME_DIR/schema-stu.log
ok=0 bad=0
pass() { echo "PASS  $*"; ok=$((ok + 1)); }
fail() { echo "FAIL  $*"; bad=$((bad + 1)); }
wait_for() { i=0; while ! eval "$1" >/dev/null 2>&1; do i=$((i + 1)); [ $i -ge "${2:-30}" ] && return 1; sleep 1; done; }
running() { [ "$(podman inspect -f '{{.State.Running}}' "$C" 2>/dev/null)" = true ]; }
scopes() { find "$SL" -maxdepth 1 -name 'podman-*.scope' 2>/dev/null | wc -l; }
sysattach() { busctl call org.freedesktop.systemd1 /org/freedesktop/systemd1 \
    org.freedesktop.systemd1.Manager AttachProcessesToUnit ssau "$@" 2>&1; }

cleanup() {
    rm -f "$Q"; systemctl --user daemon-reload > /dev/null 2>&1
    schema-ctl --user reload --evict > /dev/null 2>&1
    podman rm -f t > /dev/null 2>&1
}
trap cleanup EXIT

[ -e "$Q" ] && { echo "$Q exists; not overwriting"; trap - EXIT; exit 2; }
mkdir -p "$(dirname "$Q")"
printf '[Container]\nImage=docker.io/library/debian:trixie\nExec=sleep infinity\n[Install]\nWantedBy=default.target\n' > "$Q"
mark=$(wc -l < "$LOG" 2>/dev/null || echo 0)
systemctl --user daemon-reload > /dev/null; schema-ctl --user reload > /dev/null
# wait without podman: a shell podman would itself ask for a scope. Quadlet's
# own podman runs inside schema-user's tree (owns its cgroup) and must not.
wait_for "schema-ctl --user status $U | grep -q state=FUNDAMENTAL" 60 || { echo "container never came up"; exit 2; }
sleep 2
quadlet_pids=$(tail -n +$((mark + 1)) "$LOG" | grep -c 'scope podman-')
wait_for running 30 || { echo "container not running"; exit 2; }

out=$(podman exec "$C" sh -c 'echo exec-ok' 2>&1)
[ "$out" = exec-ok ] && pass "1 podman exec from a login shell" || fail "1 exec: $out"

podman exec "$C" sleep 3 & e=$!
sleep 1.5
n=$(scopes)
[ "$n" -ge 1 ] && pass "2 scope present during exec ($n)" || fail "2 no scope during exec"
wait $e
sleep 1; podman ps > /dev/null   # next request sweeps the finished scope
wait_for '[ "$(scopes)" -le 1 ]' 70 && pass "2 finished scopes swept ($(scopes) left)" || fail "2 $(scopes) scopes left"

r=$(busctl --user call org.freedesktop.systemd1 /org/freedesktop/systemd1 \
    org.freedesktop.systemd1.Manager StartTransientUnit 'ssa(sv)a(sa(sv))' chrome-1.scope fail 0 0 2>&1)
echo "$r" | grep -q 'not manage transient scopes' && pass "3 non-podman scope still refused" || fail "3 chrome scope: $r"

mkdir -p "$SL/podman-x.scope"
r=$(sysattach "user@$ID.service" /user.slice/podman-x.scope 1 1)
echo "$r" | grep -q 'Access denied' && pass "4 pid 1 refused" || fail "4 pid 1: $r"
r=$(sysattach user@0.service /user.slice/podman-x.scope 1 $$)
echo "$r" | grep -q 'Access denied' && pass "4 user@0 refused" || fail "4 user@0: $r"
rmdir "$SL/podman-x.scope"
r=$(sysattach "user@$ID.service" /user.slice/../.. 1 $$)
echo "$r" | grep -qi 'InvalidArgs\|not allowed' && pass "4 traversal refused" || fail "4 traversal: $r"

[ "$quadlet_pids" = 0 ] && pass "5 Quadlet start asked for no scope" || fail "5 Quadlet start logged $quadlet_pids scope requests"

podman run --rm docker.io/library/debian:trixie true && pass "6 podman run --rm" || fail "6 podman run --rm"
if podman run -d --name t docker.io/library/debian:trixie sleep 60 > /dev/null; then
    pass "6 podman run -d"
    echo "      t conmon: $(cat /proc/"$(podman inspect -f '{{.State.ConmonPid}}' t)"/cgroup)"
    echo "      t payload: $(cat /proc/"$(podman inspect -f '{{.State.Pid}}' t)"/cgroup)"
else
    fail "6 podman run -d"
fi

s=$(date +%s%N); podman ps > /dev/null; ms=$(( ($(date +%s%N) - s) / 1000000 ))
echo "      podman ps: ${ms} ms"

echo "--- $ok passed, $bad failed"
[ $bad = 0 ]
