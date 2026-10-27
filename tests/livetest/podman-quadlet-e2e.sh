#!/bin/sh
# podman-quadlet-e2e.sh — the rootless podman + Quadlet success test
# (docs/superpowers/specs/2026-10-27-rootless-podman-quadlet-design.md) on a
# live schema-user. Run as the user, with e2e-web.container installed:
#   [Container] Image=docker.io/library/nginx:alpine PublishPort=8080:80
#   Memory=64m  [Service] Restart=always  [Install] WantedBy=default.target
# $1 = a LAN host reachable over ssh, for the curl check.
set -u
U=e2e-web C=systemd-e2e-web Q=$HOME/.config/containers/systemd/$U.container
LAN=${1:-}
ok=0 bad=0
pass() { echo "PASS  $*"; ok=$((ok + 1)); }
fail() { echo "FAIL  $*"; bad=$((bad + 1)); }
wait_for() { i=0; while ! eval "$1" >/dev/null 2>&1; do i=$((i + 1)); [ $i -ge "${2:-30}" ] && return 1; sleep 1; done; }
st() { schema-ctl --user status "$U" 2>&1 | head -1; }
running() { [ "$(podman inspect -f '{{.State.Running}}' "$C" 2>/dev/null)" = true ]; }
payload() { for d in /sys/fs/cgroup/user.slice/user-$(id -u).slice/user@$(id -u).service/$U/libpod-payload-*; do [ -d "$d" ] && echo "$d"; done | head -1; }
memmax() { cat "$(payload)/memory.max" 2>/dev/null; }

[ -f "$Q" ] || { echo "no $Q"; exit 2; }
wait_for running 120 && st | grep -q 'state=FUNDAMENTAL' && pass "2 up and FUNDAMENTAL ($(st))" || fail "2 not up: $(st)"
[ "$(podman ps -q --filter name=$C | wc -l)" = 1 ] && pass "2 exactly one container" || fail "2 container count $(podman ps -q --filter name=$C | wc -l)"

[ "$(memmax)" = 67108864 ] && pass "4 payload memory.max=67108864" || fail "4 memory.max=$(memmax)"
# from a login shell podman exec is refused (crun can't move a process from
# session-N.scope into user@.service: the common ancestor is root's); systemd
# gets round it with a transient scope, a later phase here. So the hog runs
# from a throwaway user service inside the schema-user tree.
H=$XDG_RUNTIME_DIR/schema-user/services/e2e-hog.svc R=$(mktemp)
printf 'name=e2e-hog\nexec=/bin/sh\nargs=-c\nargs=podman exec %s sh -c "head -c 200m /dev/zero | tail > /dev/null"; echo $? > %s\noneshot=1\n' "$C" "$R" > "$H"
chmod 600 "$H"; : > "$R"
schema-ctl --user reload > /dev/null
wait_for "[ -s $R ]" 60; rc=$(cat "$R")
rm -f "$H" "$R"; schema-ctl --user reload --evict > /dev/null
[ "$rc" = 137 ] && pass "4 memory hog OOM-killed (rc 137)" || fail "4 memory hog rc=$rc"
wait_for "running && st | grep -q state=FUNDAMENTAL" 90

if [ -n "$LAN" ]; then
    ip=$(ip -4 route get "$(getent hosts "$LAN" | cut -d' ' -f1 || echo "$LAN")" 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
    code=$(ssh -o ConnectTimeout=5 "$LAN" "curl -s -o /dev/null -w %{http_code} http://$ip:8080/" 2>/dev/null)
    [ "$code" = 200 ] && pass "5 http://$ip:8080 from $LAN -> 200" || fail "5 from $LAN got '$code'"
else
    echo "SKIP  5 (no LAN host given)"
fi

old=$(podman inspect -f '{{.State.Pid}}' "$C")
kill -9 "$old"
wait_for "running && [ \"\$(podman inspect -f '{{.State.Pid}}' $C)\" != $old ]" 60 \
    && pass "3 back after kill -9 of the payload ($(st))" || fail "3 not back: $(st)"

sed -i 's/^Memory=64m/Memory=128m/' "$Q"
systemctl --user daemon-reload; schema-ctl --user restart "$U" > /dev/null
wait_for "running && [ \"\$(cat \"\$(payload)/memory.max\")\" = 134217728 ]" 90 \
    && pass "7 edit + daemon-reload + restart -> memory.max 134217728" || fail "7 memory.max=$(memmax)"
sed -i 's/^Memory=128m/Memory=64m/' "$Q"
systemctl --user daemon-reload; schema-ctl --user restart "$U" > /dev/null
wait_for running 90

schema-ctl --user stop "$U" > /dev/null
wait_for "[ -z \"\$(podman ps -aq --filter name=$C)\" ]" 30 \
    && pass "6 stop leaves no container" || fail "6 left: $(podman ps -a --filter name=$C --format '{{.Status}}')"

img=$(sed -n 's/^Image=//p' "$Q")
podman rmi -f "$img" > /dev/null 2>&1
schema-ctl --user start "$U" > /dev/null
wait_for running 180 && pass "cold start pulled $img and came up" || fail "cold start: $(st)"

mv "$Q" "$Q.e2e-off"
systemctl --user daemon-reload
wait_for "[ -z \"\$(podman ps -aq --filter name=$C)\" ]" 30 \
    && pass "deleted .container -> evicted, no container left" || fail "deleted .container: $(podman ps -a --filter name=$C --format '{{.Status}}')"
mv "$Q.e2e-off" "$Q"
systemctl --user daemon-reload
wait_for running 120 && pass "restored .container -> back up" || fail "restore: $(st)"

echo "== $ok passed, $bad failed"
[ $bad = 0 ]
