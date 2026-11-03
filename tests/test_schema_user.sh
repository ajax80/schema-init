#!/bin/sh
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] && { echo "test_schema_user: skipped (root)"; exit 0; }
[ -x ./schema-user ] || { echo "FAIL: no ./schema-user"; exit 1; }
[ -x ./schema-systemctl ] || { echo "FAIL: no ./schema-systemctl"; exit 1; }
T=$(mktemp -d)
export XDG_RUNTIME_DIR=$T/run XDG_STATE_HOME=$T/state/nested XDG_CONFIG_HOME=$T/cfg
# hand-written .svc files below: no import over them
export SCHEMA_USER_IMPORT=
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
PID= LPID=
cleanup() {
    [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null
    [ -n "$LPID" ] && kill -KILL "$LPID" 2>/dev/null
    pkill -KILL -f "$T/sl" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; [ -f "$T/out" ] && tail -20 "$T/out"; exit 1; }
# wait up to 5 s for a shell condition
until_ok() { i=0; while ! eval "$1"; do i=$((i + 1)); [ $i -ge 50 ] && return 1; sleep 0.1; done; }

S=$T/run/schema-user/services
mkdir -m700 "$T/run"
mkdir -p "$S" "$T/cfg/environment.d"
chmod 700 "$T/run/schema-user" "$S"
printf 'SCHEMA_T=hi-${HOME}\n# comment\n\n' > "$T/cfg/environment.d/50-t.conf"

# sleepers stamp their SIGTERM time so stop order is observable
sleeper() {
    printf 'name=%s\nexec=/bin/sh\nargs=-c\nargs=trap "date +%%s%%N > %s/%s.stop; exit 0" TERM; while :; do sleep 1 & wait; done\nstable_secs=1\n' \
        "$1" "$T" "$1"
}
sleeper a > "$S/a.svc"
{ sleeper b; echo dep=a; } > "$S/b.svc"
{ sleeper busy; echo "wait_socket=$T/run/bus"; } > "$S/busy.svc"
{ sleeper held; echo "wait_socket=$T/run/never"; } > "$S/held.svc"
printf 'name=flap\nexec=/bin/sh\nargs=-c\nargs=sleep 0.2; exit 1\n' > "$S/flap.svc"
printf 'name=dbl\nexec=/bin/sh\nargs=-c\nargs=sh -c '"'"'sleep 0.5; cut -d" " -f4 /proc/$$/stat > %s/dbl.ppid; exec %s/sl 30'"'"' & exit 0\noneshot=1\n' \
    "$T" "$T" > "$S/dbl.svc"
printf 'name=envdump\nexec=/bin/sh\nargs=-c\nargs=env > %s/env\noneshot=1\n' "$T" > "$S/envdump.svc"
printf 'name=tmr\nexec=/bin/echo\nargs=tick\non_calendar=03:00\npersistent=1\n' > "$S/tmr.svc"
# exec_stop / exec_stop_post: stamped so order against SIGTERM is observable
{ sleeper sc; printf 'exec_stop=/bin/sh -c "date +%%s%%N > %s/sc.stopcmd"\nexec_stop_post=/bin/sh -c "date +%%s%%N > %s/sc.post"\n' "$T" "$T"; } > "$S/sc.svc"
{ sleeper cr; printf 'exec_stop_post=/bin/sh -c "date +%%s%%N > %s/cr.post"\nno_restart=1\n' "$T"; } > "$S/cr.svc"
{ sleeper hg; printf 'exec_stop=/bin/sleep 100\nstop_timeout_sec=2\n'; } > "$S/hg.svc"
{ sleeper sd; printf 'exec_stop=/bin/sh -c "date +%%s%%N > %s/sd.stopcmd"\nexec_stop_post=/bin/sh -c "date +%%s%%N > %s/sd.post"\n' "$T" "$T"; } > "$S/sd.svc"
# exec_stop kills main itself and outlives it: exec_stop_post must still run (#433)
printf 'name=pk\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/pk.pid; trap "exit 0" TERM; while :; do sleep 1 & wait; done\nstable_secs=1\nexec_stop=/bin/sh -c "kill $(cat %s/pk.pid); sleep 1"\nexec_stop_post=/bin/sh -c "date +%%s%%N > %s/pk.post"\n' "$T" "$T" "$T" > "$S/pk.svc"
# ...and at shutdown too (M2): exec_stop kills main and outlives it, no SIGTERM is ever sent
printf 'name=pks\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/pks.pid; trap "exit 0" TERM; while :; do sleep 1 & wait; done\nstable_secs=1\nexec_stop=/bin/sh -c "kill $(cat %s/pks.pid); sleep 1; date +%%s%%N > %s/pks.stopcmd"\nexec_stop_post=/bin/sh -c "date +%%s%%N > %s/pks.post"\n' "$T" "$T" "$T" "$T" > "$S/pks.svc"
# ...and when that exec_stop then hangs: held for its grace (2 s) only, killed, post still runs
printf 'name=pkh\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/pkh.pid; trap "exit 0" TERM; while :; do sleep 1 & wait; done\nstable_secs=1\nexec_stop=/bin/sh -c "kill $(cat %s/pkh.pid); exec %s/sl 100"\nexec_stop_post=/bin/sh -c "date +%%s%%N > %s/pkh.post"\nstop_timeout_sec=2\n' "$T" "$T" "$T" "$T" > "$S/pkh.svc"
# ...and when main exits on its own during shutdown, before its turn (blocked by a dependent)
printf 'name=sx\nexec=/bin/sh\nargs=-c\nargs=trap "" TERM; while [ ! -e %s/sx.go ]; do sleep 0.1; done\nstable_secs=1\nexec_stop_post=/bin/sh -c "date +%%s%%N > %s/sx.post"\n' "$T" "$T" > "$S/sx.svc"
{ sleeper sxd; printf 'dep=sx\nexec_stop=/bin/sh -c "touch %s/sx.go; sleep 1"\n' "$T"; } > "$S/sxd.svc"
# a timer run gets exec_stop_post too (#433)
printf 'name=tp\nexec=/bin/true\non_boot_sec=0\nexec_stop_post=/bin/sh -c "date +%%s%%N > %s/tp.post"\n' "$T" > "$S/tp.svc"
chmod 600 "$S"/*.svc
# the orphan runs as $T/sl so its cmdline is unique to this run
ln -s "$(command -v sleep)" "$T/sl"

./schema-user > "$T/out" 2>&1 &
PID=$!

until_ok '[ -S "$T/run/schema-user/ctl.sock" ]' || fail "no ctl.sock"
[ "$(stat -c %a "$T/run/schema-user/ctl.sock")" = 600 ] || fail "ctl.sock mode $(stat -c %a "$T/run/schema-user/ctl.sock")"
st() { ./schema-ctl --user status 2>&1; }
until_ok 'st | grep -Eq "^ +a +pid=[1-9][0-9]* +state=FUNDAMENTAL" && st | grep -Eq "^ +b +pid=[1-9][0-9]* +state=FUNDAMENTAL"' \
    || { st; fail "a/b not FUNDAMENTAL"; }
until_ok 'st | grep -Eq "^ +flap .* restarts=[1-9]"' || { st; fail "flap never restarted"; }

until_ok '[ -s "$T/env" ]' || fail "envdump did not run"
grep -q '^HOME=' "$T/env" || fail "no HOME"
grep -qx "XDG_RUNTIME_DIR=$T/run" "$T/env" || fail "XDG_RUNTIME_DIR"
grep -qx "DBUS_SESSION_BUS_ADDRESS=unix:path=$T/run/bus" "$T/env" || fail "DBUS_SESSION_BUS_ADDRESS"
grep -qx "SCHEMA_T=hi-$HOME" "$T/env" || fail "environment.d: $(grep SCHEMA_T "$T/env")"
grep -q '^WAYLAND_DISPLAY=' "$T/env" && fail "WAYLAND_DISPLAY leaked"

until_ok '[ -s "$T/dbl.ppid" ]' || fail "dbl grandchild never wrote ppid"
[ "$(cat "$T/dbl.ppid")" = "$PID" ] || fail "orphan reparented to $(cat "$T/dbl.ppid"), not schema-user $PID"

[ -f "$T/state/nested/schema-user/timers/tmr.stamp" ] || fail "no timer stamp"
[ -f "$T/state/nested/schema-user/log/a.log" ] || fail "no service log"
SHM=/dev/shm/schema-user-$(id -u)
[ "$(stat -c %a "$SHM")" = 600 ] || fail "shm $SHM mode $(stat -c %a "$SHM")"

# a user may edit their own .svc and reload it; PID 1's since-boot rule is not theirs
echo "stable_secs=2" >> "$S/a.svc"
./schema-ctl --user reload > "$T/reload" 2>&1 || fail "reload of an edited .svc: $(cat "$T/reload")"

./schema-user > "$T/second" 2>&1 && fail "second instance started"
grep -q "already running" "$T/second" || fail "second instance: $(cat "$T/second")"
env -u XDG_RUNTIME_DIR ./schema-user 2>/dev/null && fail "ran without XDG_RUNTIME_DIR"
XDG_RUNTIME_DIR=/nonexistent ./schema-user 2>/dev/null && fail "ran with a missing XDG_RUNTIME_DIR"

./schema-ctl --user stop flap > /dev/null || fail "stop flap"

pid_of() { st | sed -n "s/^ *$1 *pid=\([0-9]*\) .*/\1/p"; }
until_ok '[ -n "$(pid_of sc)" ] && [ "$(pid_of sc)" != 0 ] && [ -n "$(pid_of cr)" ] && [ "$(pid_of cr)" != 0 ] && [ "$(pid_of hg)" != 0 ]' \
    || { st; fail "sc/cr/hg not running"; }
./schema-ctl --user stop sc > /dev/null || fail "stop sc"
until_ok '[ -s "$T/sc.stop" ] && [ -s "$T/sc.post" ]' || { st; fail "sc: stop/post never ran"; }
[ -s "$T/sc.stopcmd" ] || fail "sc: exec_stop never ran"
[ "$(cat "$T/sc.stopcmd")" -le "$(cat "$T/sc.stop")" ] || fail "sc: exec_stop ran after SIGTERM"
[ "$(cat "$T/sc.stop")" -le "$(cat "$T/sc.post")" ] || fail "sc: exec_stop_post ran before the service stopped"
kill -KILL "$(pid_of cr)"
until_ok '[ -s "$T/cr.post" ]' || { st; fail "cr: exec_stop_post not run after a crash"; }
ctxsw() { awk '/^voluntary_ctxt_switches/ {print $2}' "/proc/$PID/status"; }

# wait_socket=: held while the socket is missing, and while it is stale
busy_waiting() { st | grep -Eq "^ +busy +pid=0 .*  waiting=$T/run/bus\$"; }
sleep 2
busy_waiting || { st; fail "busy not held on a missing socket"; }
python3 -c 'import socket, sys; s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.close()' "$T/run/bus"
[ -S "$T/run/bus" ] || fail "no stale socket made"
c0=$(ctxsw)
sleep 2
c1=$(ctxsw)
# a held service is re-checked once a second, not on the 250 ms tick (8)
[ $((c1 - c0)) -le 4 ] || fail "held service ticks: $((c1 - c0)) wakeups in 2 s"
busy_waiting || { st; fail "busy not held on a stale socket"; }
python3 -c 'import os, socket, sys, time
os.unlink(sys.argv[1]); s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(8); time.sleep(30)' "$T/run/bus" &
LPID=$!
until_ok 'st | grep -Eq "^ +busy +pid=[1-9][0-9]* +state=(FULL_TRUST|FUNDAMENTAL)"' \
    || { st; fail "busy not started once the socket accepts"; }
until_ok 'st | grep -Eq "^ +busy +pid=[1-9][0-9]* +state=FUNDAMENTAL"' || { st; fail "busy not FUNDAMENTAL"; }
st | grep "waiting=" | grep -vq "^ *held " && { st; fail "waiting= still shown"; }
# a held service that is stopped is no longer waiting, and no longer wakes us
./schema-ctl --user stop held > /dev/null || fail "stop held"
st | grep -q "waiting=" && { st; fail "stopped service still shown waiting"; }

sleep 0.5
c0=$(ctxsw)
sleep 3
c1=$(ctxsw)
# idle measures 0; a 250 ms tick is 12
[ $((c1 - c0)) -le 3 ] || fail "not idle: $((c1 - c0)) wakeups in 3 s"
# a hung exec_stop on an idle supervisor is still timed out (#433: poll never woke)
t0=$(date +%s%N)
./schema-ctl --user stop hg > /dev/null || fail "stop hg"
st > /dev/null
[ $(( ($(date +%s%N) - t0) / 1000000 )) -lt 1000 ] || fail "status blocked behind a hung exec_stop"
# no status polling here: each ctl request wakes the loop and would hide it
sleep 4
pgrep -f "^/bin/sleep 100" > /dev/null && fail "hung exec_stop not killed after stop_timeout_sec on an idle loop"
[ "$(pid_of hg)" = 0 ] || { st; fail "hg still running 4 s after stop with a hung exec_stop"; }
./schema-ctl --user stop pk > /dev/null
until_ok '[ -s "$T/pk.post" ]' || { st; fail "pk: exec_stop_post skipped when exec_stop outlived main"; }
until_ok '[ -s "$T/tp.post" ]' || { st; fail "tp: timer run got no exec_stop_post"; }

kill -TERM "$PID"
until_ok '! kill -0 "$PID" 2>/dev/null' || fail "still running 5 s after SIGTERM"
wait "$PID"; rc=$?
PID=
[ $rc = 0 ] || fail "exit status $rc"
[ -s "$T/a.stop" ] && [ -s "$T/b.stop" ] || fail "sleepers saw no SIGTERM"
[ "$(cat "$T/b.stop")" -le "$(cat "$T/a.stop")" ] || fail "a stopped before its dependent b"
[ -s "$T/sd.stopcmd" ] || fail "sd: exec_stop not run at shutdown"
[ -s "$T/pks.post" ] || fail "pks: exec_stop_post skipped at shutdown when exec_stop took main down"
[ "$(cat "$T/pks.stopcmd")" -le "$(cat "$T/pks.post")" ] || fail "pks: exec_stop_post ran before exec_stop finished"
[ -s "$T/sx.post" ] || fail "sx: exec_stop_post skipped when main exited on its own during shutdown"
[ -s "$T/pkh.post" ] || fail "pkh: exec_stop_post skipped after a hung exec_stop was killed"
[ -s "$T/sd.post" ] || fail "sd: exec_stop_post not run at shutdown"
[ "$(cat "$T/sd.stopcmd")" -le "$(cat "$T/sd.stop")" ] || fail "sd: exec_stop ran after SIGTERM at shutdown"
pgrep -f "$T/sl" > /dev/null && fail "dbl orphan survived"
# --- schema-user runs schema-import --user at start (SCHEMA_USER_IMPORT) ---
# start a fresh instance on its own tree; $1 = importer path ("" = none)
istart() {
    I=$(mktemp -d "$T/imp.XXXX")
    mkdir -m700 "$I/run"
    SCHEMA_USER_IMPORT=$1 XDG_RUNTIME_DIR=$I/run XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg \
        ./schema-user > "$I/out" 2>&1 &
    PID=$!
}
ist() { XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status 2>/dev/null; }
istop() { kill -TERM "$PID"; wait "$PID"; PID=; }
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$T/$1"; chmod 755 "$T/$1"; echo "$T/$1"; }

# import_runs: the stub's .svc lands in --out and runs
S1=$(stub imp-ok 'while [ $# -gt 0 ]; do [ "$1" = --out ] && o=$2; shift; done
printf "name=x\nexec=%s\nargs=30\n" "'"$T/sl"'" > "$o/x.svc"; chmod 600 "$o/x.svc"')
istart "$S1"
until_ok 'ist | grep -Eq "^ +x +pid=[1-9]"' || { ist; cat "$I/out"; fail "import_runs: x not running"; }
grep -q "import: exit 0" "$I/out" || fail "import_runs: no exit line"
istop

# import_fail: exit 1 -> still serves status
S2=$(stub imp-fail 'exit 1')
istart "$S2"
until_ok 'ist | grep -q "^services:"' || fail "import_fail: no status"
grep -q "import: exit 1" "$I/out" || fail "import_fail: no exit 1 line"
istop

# import_hang: killed after 10 s, then serves status
S3=$(stub imp-hang "exec $T/sl 60")
istart "$S3"
i=0; until ist | grep -q "^services:"; do i=$((i + 1)); [ $i -ge 130 ] && fail "import_hang: no status in 13 s"; sleep 0.1; done
grep -q "import: killed after 10 s" "$I/out" || fail "import_hang: no kill line"
pgrep -f "$T/sl 60" > /dev/null && fail "import_hang: importer left running"
istop

# import_real: the real importer, under schema-user's own environment, turns
# an enabled unit in $XDG_CONFIG_HOME/systemd/user into a running service
I=$(mktemp -d "$T/imp.XXXX"); mkdir -m700 "$I/run"
mkdir -p "$I/cfg/systemd/user/default.target.wants"
printf '[Service]\nExecStart=%s 31\nRestart=always\n' "$T/sl" > "$I/cfg/systemd/user/e2e.service"
ln -s ../e2e.service "$I/cfg/systemd/user/default.target.wants/e2e.service"
install -m755 distros/fedora-installer/migrate/schema-import.py "$T/schema-import"  # 0755 as packaged
SCHEMA_USER_IMPORT=$T/schema-import XDG_RUNTIME_DIR=$I/run \
    XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg ./schema-user > "$I/out" 2>&1 &
PID=$!
until_ok 'ist | grep -Eq "^ +e2e +pid=[1-9]"' || { ist; cat "$I/out" "$I/state/schema-user/log/import.log"; fail "import_real: e2e not running"; }
grep -q "import: exit 0" "$I/out" || fail "import_real: importer did not exit 0"
# systemctl --user enable --now: link, daemon-reload (re-import), start
printf '[Service]\nExecStart=%s 32\n[Install]\nWantedBy=default.target\n' "$T/sl" > "$I/cfg/systemd/user/e2f.service"
usc() { SCHEMA_CTL=./schema-ctl XDG_RUNTIME_DIR=$I/run XDG_CONFIG_HOME=$I/cfg ./schema-systemctl --user "$@"; }
usc enable --now e2f.service 2>/dev/null || fail "enable --now e2f: exit $?"
[ -L "$I/cfg/systemd/user/default.target.wants/e2f.service" ] || fail "enable: no .wants link"
until_ok 'ist | grep -Eq "^ +e2f +pid=[1-9]"' || { ist; cat "$I/out"; fail "enable --now: e2f not running"; }
usc is-active e2f > /dev/null || fail "is-active e2f"
usc is-enabled e2f > /dev/null || fail "is-enabled e2f"
usc disable --now e2f 2>/dev/null || fail "disable --now e2f"
ist | grep -q e2f && { ist; fail "disable: e2f still in the table"; }
until_ok '! pgrep -f "$T/sl 32" > /dev/null' || fail "disable --now: e2f still running"
# without --now too: the evicting reload stops it, never left running unmanaged
usc enable --now e2f.service 2>/dev/null || fail "re-enable e2f"
until_ok 'ist | grep -Eq "^ +e2f +pid=[1-9]"' || fail "re-enable: e2f not running"
usc disable e2f 2>/dev/null || fail "disable e2f"
until_ok '! pgrep -f "$T/sl 32" > /dev/null' || fail "disable: e2f left running"
ist | grep -Eq "^ +e2e +pid=[1-9]" || fail "daemon-reload: e2e lost"
istop

# import_disabled: empty SCHEMA_USER_IMPORT keeps a hand-written .svc
I0=$(mktemp -d "$T/imp.XXXX"); mkdir -m700 "$I0/run" "$I0/run/schema-user"; mkdir -m700 "$I0/run/schema-user/services"
printf 'name=h\nexec=%s\nargs=30\n' "$T/sl" > "$I0/run/schema-user/services/h.svc"; chmod 600 "$I0/run/schema-user/services/h.svc"
I=$I0
SCHEMA_USER_IMPORT= XDG_RUNTIME_DIR=$I/run XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg ./schema-user > "$I/out" 2>&1 &
PID=$!
until_ok 'ist | grep -Eq "^ +h +pid=[1-9]"' || { ist; fail "import_disabled: h not running"; }
grep -q "import: not run" "$I/out" || fail "import_disabled: no 'not run' line"
# status lists the skip manifest (text, --kv and --json)
printf 'syncthing.service\thandled elsewhere (skip list)\n' > "$I/run/schema-user/skipped"
ist | grep -Eq '^ +syncthing\.service +skipped \(handled elsewhere \(skip list\)\)$' || { ist; fail "status: no skipped line"; }
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --kv | grep -qx 'service.syncthing.service.state=skipped' || fail "status --kv: no skipped state"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --kv | grep -qx 'service.syncthing.service.reason=handled elsewhere (skip list)' || fail "status --kv: no reason"
jcheck() {
    XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --json | python3 -c '
import json, sys
d = json.load(sys.stdin)
# a running service'"'"'s state moves on its own: only skipped ones are pinned
got = sorted((s["name"], s["state"] if s["state"] == "skipped" else "up", s.get("reason"))
             for s in d["services"])
want = eval(sys.argv[1])
assert got == want, got' "$1" || fail "status --json: wanted $1"
}
jcheck '[("h", "up", None), ("syncthing.service", "skipped", "handled elsewhere (skip list)")]'
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status h | grep -q syncthing && fail "status <svc>: lists skipped"
# daemon-reload to zero services is a normal user state, not a broken config
rm "$I/run/schema-user/services/h.svc"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user daemon-reload | grep -qx "ok: daemon-reload" || fail "daemon-reload to empty"
jcheck '[("syncthing.service", "skipped", "handled elsewhere (skip list)")]'
# ...but a missing services dir is a half-done import, refused
mv "$I/run/schema-user/services" "$I/run/schema-user/gone"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user daemon-reload > /dev/null && fail "daemon-reload of a missing dir"
mv "$I/run/schema-user/gone" "$I/run/schema-user/services"
istop

# persistent catch-up waits for mount-fstab: it "mounts" /var by dropping in
# a 3-day-old stamp; read before that, the stamp looks new and cu never runs
I=$(mktemp -d "$T/imp.XXXX"); mkdir -m700 "$I/run" "$I/run/schema-user" "$I/run/schema-user/services"
old=$(( $(date +%s) - 3 * 86400 ))
printf 'name=mount-fstab\nexec=/bin/sh\nargs=-c\nargs=sleep 1; mkdir -p %s/state/schema-user/timers; echo %s > %s/state/schema-user/timers/cu.stamp\noneshot=1\n' \
    "$I" "$old" "$I" > "$I/run/schema-user/services/mount-fstab.svc"
printf 'name=cu\nexec=/bin/sh\nargs=-c\nargs=touch %s/cu.ran\non_calendar=03:00\npersistent=1\n' "$I" > "$I/run/schema-user/services/cu.svc"
chmod 600 "$I/run/schema-user/services"/*.svc
SCHEMA_USER_IMPORT= XDG_RUNTIME_DIR=$I/run XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg ./schema-user > "$I/out" 2>&1 &
PID=$!
until_ok '[ -f "$I/cu.ran" ]' || { ist; cat "$I/out"; fail "catch-up: cu did not run after mount-fstab"; }
# the stamp lands after the reap, a beat behind cu.ran
until_ok '[ "$(cat "$I/state/schema-user/timers/cu.stamp")" -gt "$old" ]' || fail "catch-up: stamp not advanced"
istop
# ...and a mount-fstab that keeps failing releases them after its first attempt
sed -i 's/; echo \([0-9]*\) > \(.*cu.stamp\)$/; echo \1 > \2; exit 1/' "$I/run/schema-user/services/mount-fstab.svc"
sed -i '/^oneshot=1$/d' "$I/run/schema-user/services/mount-fstab.svc"
rm -f "$I/cu.ran" "$I/state/schema-user/timers/cu.stamp"
SCHEMA_USER_IMPORT= XDG_RUNTIME_DIR=$I/run XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg ./schema-user > "$I/out" 2>&1 &
PID=$!
until_ok '[ -f "$I/cu.ran" ]' || { ist; cat "$I/run/schema-user/services/mount-fstab.svc" "$I/out"; fail "catch-up: held by a failing mount-fstab"; }
istop

# excise_reason: each excise names its cause in status (text, --kv, --json)
I=$(mktemp -d "$T/imp.XXXX"); mkdir -m700 "$I/run" "$I/run/schema-user" "$I/run/schema-user/services"
X=$I/run/schema-user/services
printf 'name=xc\nexec=/bin/sh\nargs=-c\nargs=exit 3\nno_restart=1\n' > "$X/xc.svc"
printf 'name=xs\nexec=/bin/sh\nargs=-c\nargs=kill -TERM $$\nno_restart=1\n' > "$X/xs.svc"
printf 'name=xok\nexec=/bin/sh\nargs=-c\nargs=exit 0\nno_restart=1\n' > "$X/xok.svc"
printf 'name=xt\nexec=%s\nargs=30\nstable_secs=30\nstart_timeout_sec=1\n' "$T/sl" > "$X/xt.svc"
: > "$I/notadir"
printf 'name=xl\nexec=%s\nargs=30\nlisten=stream:%s/notadir/x.sock\nlisten_lazy=1\n' "$T/sl" "$I" > "$X/xl.svc"
chmod 600 "$X"/*.svc
SCHEMA_USER_IMPORT= XDG_RUNTIME_DIR=$I/run XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg ./schema-user > "$I/out" 2>&1 &
PID=$!
kv() { XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --kv 2>/dev/null; }
for want in xc=exit-code xs=signal xok=none xt=start-timeout xl=listen-fail; do
    n=${want%%=*}
    until_ok 'kv | grep -qx "service.$n.state=EXCISED"' || { kv; cat "$I/out"; fail "excise_reason: $n never excised"; }
    kv | grep -qx "service.$n.excise_reason=${want#*=}" || { kv | grep "^service.$n\."; fail "excise_reason: $n wanted ${want#*=}"; }
done
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status xt | grep -qx 'excise_reason=start-timeout' || fail "status xt: no excise_reason line"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --json | python3 -c '
import json, sys
r = {s["name"]: s.get("excise_reason") for s in json.load(sys.stdin)["services"]}
assert r["xc"] == "exit-code" and r["xok"] == "none", r' || fail "status --json: excise_reason"
# start clears it: with a socket dir it can bind in, xl runs and nothing re-excises it
rm "$I/notadir"; mkdir "$I/notadir"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user start xl > /dev/null || fail "start xl"
until_ok 'kv | grep -Eqx "service.xl.pid=[1-9][0-9]*"' || { kv | grep "^service.xl\."; fail "start xl: not running"; }
kv | grep -qx "service.xl.excise_reason=none" || { kv | grep "^service.xl\."; fail "start xl: reason not cleared"; }
istop

# no_restart=1 survives schema-ctl stop + start: start used to clear it with the stop's hold
I=$(mktemp -d "$T/imp.XXXX"); mkdir -m700 "$I/run" "$I/run/schema-user" "$I/run/schema-user/services"
printf 'name=nr\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/nr.pid; date +%%s%%N >> %s/nr.starts; trap "exit 0" TERM; while :; do sleep 1 & wait; done\nstable_secs=1\nno_restart=1\n' \
    "$I" "$I" > "$I/run/schema-user/services/nr.svc"
chmod 600 "$I/run/schema-user/services/nr.svc"
SCHEMA_USER_IMPORT= XDG_RUNTIME_DIR=$I/run XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg ./schema-user > "$I/out" 2>&1 &
PID=$!
nk() { XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --kv 2>/dev/null | sed -n "s/^service\.nr\.$1=//p"; }
until_ok '[ "$(nk state)" = FUNDAMENTAL ]' || fail "no_restart: nr never up"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user stop nr > /dev/null
until_ok '[ "$(nk state)" = EXCISED ]' || fail "no_restart: stop nr"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user start nr > /dev/null
until_ok '[ "$(nk state)" = FUNDAMENTAL ] && [ "$(wc -l < "$I/nr.starts")" = 2 ]' || fail "no_restart: start nr"
kill -KILL "$(cat "$I/nr.pid")"
until_ok '[ "$(nk state)" = EXCISED ]' || fail "no_restart: nr not excised after its death"
sleep 1.5
[ "$(wc -l < "$I/nr.starts")" = 2 ] || fail "no_restart=1 lost after stop+start: nr respawned"
istop

# bind= (docs/superpowers/specs/2026-11-03-bind-key-design.md): ctr follows pod.
# pod's exec_stop ends ctr's main first, as podman pod stop does (the race in review #1)
I=$(mktemp -d "$T/imp.XXXX"); mkdir -m700 "$I/run" "$I/run/schema-user" "$I/run/schema-user/services"
B=$I/run/schema-user/services
bsvc() {  # name, extra lines
    printf 'name=%s\nexec=/bin/sh\nargs=-c\nargs=echo $$ > %s/%s.pid; date +%%s%%N >> %s/%s.starts; trap "exit 0" TERM; while :; do sleep 1 & wait; done\nstable_secs=1\n%b' \
        "$1" "$I" "$1" "$I" "$1" "$2" > "$B/$1.svc"
}
# ...and outlasts BIND_GRACE_MS, so only the stop-request paths (not the
# crash reclassification) can call ctr's exit the pod's doing
bsvc pod "exec_stop=/bin/sh -c \"kill \$(cat $I/ctr.pid) 2>/dev/null; sleep 2.5\"\n"
bsvc ctr "bind=pod\nno_restart=1\nexec_stop_post=/bin/sh -c \"date +%s%N >> $I/ctr.post\"\n"
# case 6: a binder whose exec_stop outlives its pod's restart
bsvc pod2 ""
bsvc slow "bind=pod2\nno_restart=1\nexec_stop=/bin/sh -c \"sleep 2; date +%s%N > $I/slow.stopdone\"\n"
chmod 600 "$B"/*.svc
SCHEMA_USER_IMPORT= XDG_RUNTIME_DIR=$I/run XDG_STATE_HOME=$I/state XDG_CONFIG_HOME=$I/cfg ./schema-user > "$I/out" 2>&1 &
PID=$!
bk() { XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status --kv 2>/dev/null | sed -n "s/^service\.$1\.$2=//p"; }
bc() { XDG_RUNTIME_DIR=$I/run ./schema-ctl --user "$@" > /dev/null 2>&1; }
starts() { wc -l < "$I/$1.starts" 2>/dev/null || echo 0; }
up() { until_ok '[ "$(bk '"$1"' state)" = FUNDAMENTAL ]' || { cat "$I/out" | tail -20; fail "bind: $1 never FUNDAMENTAL ($2)"; }; }
up pod start; up ctr start; up pod2 start; up slow start
[ "$(bk ctr bind)" = pod ] && [ "$(bk ctr bind_held)" = 0 ] || fail "bind: --kv bind/bind_held"

# 1. stop pod -> ctr held (EXCISED, excise_reason=bind), its post ran, no restart counted
bc stop pod
until_ok '[ "$(bk ctr excise_reason)" = bind ]' || { bk ctr state; tail -20 "$I/out"; fail "bind 1: ctr not held after stop pod"; }
[ "$(bk ctr state)" = EXCISED ] && [ "$(bk ctr bind_held)" = 1 ] && [ "$(bk ctr restarts)" = 0 ] || fail "bind 1: state/held/restarts"
until_ok '[ -s "$I/ctr.post" ]' || fail "bind 1: ctr exec_stop_post skipped"
XDG_RUNTIME_DIR=$I/run ./schema-ctl --user status ctr | grep -qx "bound to pod (held)" || fail "bind 1: status line"
until_ok '[ "$(bk pod state)" = EXCISED ]' || fail "bind 1: pod never stopped"
sleep 1; [ "$(starts ctr)" = 1 ] || fail "bind 1: ctr restarted while pod is stopping or down"

# 2. start pod -> ctr back
bc start pod
until_ok '[ "$(starts ctr)" = 2 ] && [ "$(bk ctr state)" = FUNDAMENTAL ]' || { tail -20 "$I/out"; fail "bind 2: ctr not back with pod"; }
[ "$(bk ctr bind_held)" = 0 ] && [ "$(bk ctr excise_reason)" = none ] || fail "bind 2: held/reason not cleared"

# 3. ctr stopped on purpose: a pod restart leaves it down; start brings it back
bc stop ctr
until_ok '[ "$(bk ctr state)" = EXCISED ]' || fail "bind 3: stop ctr"
p0=$(starts pod); bc restart pod
until_ok '[ "$(starts pod)" -gt '"$p0"' ] && [ "$(bk pod state)" = FUNDAMENTAL ]' || fail "bind 3: pod restart"
sleep 1.5
[ "$(starts ctr)" = 2 ] && [ "$(bk ctr bind_held)" = 0 ] || fail "bind 3: user-stopped ctr came back with pod"
bc start ctr; until_ok '[ "$(starts ctr)" = 3 ]' || fail "bind 3: start ctr"
up ctr "3 start"

# 4. crash: ctr dies first, then pod's main -> reclassified, back with pod, no restart counted
kill -KILL "$(cat "$I/ctr.pid")"; sleep 0.1; kill -KILL "$(cat "$I/pod.pid")"
until_ok 'grep -q "ctr .*bind-reclassified" "$I/out"' || { grep -E "\] (ctr|pod) " "$I/out" | tail -25; fail "bind 4: ctr's exit not reclassified"; }
until_ok '[ "$(starts ctr)" = 4 ] && [ "$(bk ctr state)" = FUNDAMENTAL ]' || { tail -20 "$I/out"; fail "bind 4: ctr not back after pod crash"; }
[ "$(bk ctr restarts)" = 0 ] || fail "bind 4: reclassified exit counted as a restart ($(bk ctr restarts))"

# 5. ctr dies on its own, pod stays up past the 2 s grace -> not held, stays down
kill -KILL "$(cat "$I/ctr.pid")"
until_ok '[ "$(bk ctr state)" = EXCISED ]' || fail "bind 5: ctr not excised"
sleep 2.5
[ "$(bk ctr excise_reason)" = signal ] && [ "$(bk ctr bind_held)" = 0 ] && [ "$(starts ctr)" = 4 ] || \
    fail "bind 5: own death treated as bind ($(bk ctr excise_reason), held=$(bk ctr bind_held))"

# 6. pod2 restarts while slow's exec_stop (2 s) runs: slow requeued, spawned only after it
bc restart pod2
until_ok '[ "$(starts slow)" = 2 ]' || { tail -20 "$I/out"; fail "bind 6: slow not back after pod2 restart"; }
[ -s "$I/slow.stopdone" ] && [ "$(tail -1 "$I/slow.starts")" -ge "$(cat "$I/slow.stopdone")" ] || \
    fail "bind 6: slow respawned before its exec_stop finished"

# 8 (review #1). slow is being stopped on purpose (exec_stop 2 s) when pod2 restarts: stays down
s0=$(starts slow); bc stop slow; bc restart pod2
until_ok '[ "$(bk slow state)" = EXCISED ] && [ "$(bk pod2 state)" = FUNDAMENTAL ]' || fail "bind 8: settle"
sleep 1.5
[ "$(starts slow)" = "$s0" ] && [ "$(bk slow bind_held)" = 0 ] || fail "bind 8: a user stop became a bind hold"
bc start slow; until_ok '[ "$(bk slow state)" = FUNDAMENTAL ]' || fail "bind 8: start slow"

# 9 (review #2). pod dies first and respawns, ctr dies after: held against the run that
# went down, so the new run being up brings it back
bc start ctr; up ctr "9 pre"
c0=$(starts ctr)
kill -KILL "$(cat "$I/pod.pid")"; sleep 0.2; kill -KILL "$(cat "$I/ctr.pid")" 2>/dev/null
until_ok '[ "$(starts ctr)" -gt '"$c0"' ] && [ "$(bk ctr state)" = FUNDAMENTAL ]' || \
    { grep -E "\] (ctr|pod) " "$I/out" | tail -12; fail "bind 9: ctr stuck held after pod respawned first"; }

# 10 (review #7). a clean exit is its own: a pod restart right after doesn't revive it
kill -TERM "$(cat "$I/ctr.pid")"
until_ok '[ "$(bk ctr state)" = EXCISED ]' || fail "bind 10: ctr clean exit"
c0=$(starts ctr); p0=$(starts pod); bc restart pod
until_ok '[ "$(starts pod)" -gt '"$p0"' ] && [ "$(bk pod state)" = FUNDAMENTAL ]' || fail "bind 10: pod restart"
sleep 1.5
[ "$(starts ctr)" = "$c0" ] && [ "$(bk ctr excise_reason)" = none ] || fail "bind 10: clean exit reclassified as the pod's"

# 11 (review #4). bind= removed while held -> released and started
bc start ctr; until_ok '[ "$(bk ctr state)" = FUNDAMENTAL ]' || fail "bind 11: pre"
bc stop pod; until_ok '[ "$(bk ctr bind_held)" = 1 ] && [ "$(bk pod state)" = EXCISED ]' || fail "bind 11: ctr not held"
sed -i '/^bind=pod$/d' "$B/ctr.svc"; c0=$(starts ctr); bc daemon-reload
until_ok '[ "$(starts ctr)" -gt '"$c0"' ]' || { grep -E "\] ctr " "$I/out" | tail -8; fail "bind 11: ctr still held with bind= gone"; }
printf 'bind=pod\n' >> "$B/ctr.svc"; bc start pod; bc daemon-reload
until_ok '[ "$(bk pod state)" = FUNDAMENTAL ] && [ "$(bk ctr bind)" = pod ]' || fail "bind 11: restore"

# 7. reload without pod -> ctr held, bind-target-missing
bc start ctr; up ctr "7 pre"
rm "$B/pod.svc"; bc daemon-reload
until_ok '[ "$(bk ctr bind_held)" = 1 ]' || { tail -20 "$I/out"; fail "bind 7: ctr not held with pod gone"; }
grep -q "ctr .*bind-target-missing" "$I/out" || fail "bind 7: no bind-target-missing"
istop

echo "test_schema_user: ok"
