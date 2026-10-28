#!/bin/sh
set -u
cd "$(dirname "$0")/.."
BOOTOK=$PWD/scripts/schema-pi-bootok
DEADLINE=$PWD/scripts/schema-pi-deadline
T=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

R=$T/root
STATE=$R/var/lib/schema-init/pi-boot
KG=$R/var/lib/schema-init/known-good
MARK=$R/run/schema-init/pi-boot-ok

cat > "$T/ctl" <<EOT
#!/bin/sh
case "\$1" in
status) cat "$T/kv" ;;
reboot) echo reboot >> "$T/reboots" ;;
esac
EOT
chmod +x "$T/ctl"

reset() {
    rm -rf "$R" "$T/kv" "$T/reboots" "$T/log"
    mkdir -p "$R/usr/bin" "$R/etc/schema-init/services" "$R/usr/libexec/schema-init" \
        "$R/var/lib/schema-init"
    echo bin-init > "$R/usr/bin/schema-init"
    echo bin-ctl > "$R/usr/bin/schema-ctl"
    echo svc > "$R/etc/schema-init/services/sshd.svc"
    echo b > "$R/usr/libexec/schema-init/schema-pi-bootok"
    echo d > "$R/usr/libexec/schema-init/schema-pi-deadline"
    printf 'attempts=2\nlast_target=%s\n' "${1:-current}" > "$STATE"
}

env_run() {
    SCHEMA_PI_ROOT=$R SCHEMA_CTL=$T/ctl SCHEMA_PI_POLL=0.1 "$@"
}

good_kv() {
    printf 'service.sshd.state=FUNDAMENTAL\nservice.net.state=PERFECT\nservice.pi-bootok.state=NEW_PROCESS\nservice.pi-deadline.state=NEW_PROCESS\nservice.gone.state=skipped\nservice.gone.reason=no exec\n' > "$T/kv"
}

# bootok_confirms
reset; good_kv
env_run sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_confirms: exit"
[ -f "$MARK" ] || fail "bootok_confirms: no marker"
[ "$(cat "$STATE")" = "attempts=0
last_target=current" ] || fail "bootok_confirms: state: $(cat "$STATE")"
[ -f "$KG/schema-init" ] && [ -f "$KG/schema-ctl" ] && [ -f "$KG/services/sshd.svc" ] || fail "bootok_confirms: kg not populated"
[ -f "$KG/schema-pi-bootok" ] && [ -f "$KG/schema-pi-deadline" ] || fail "bootok_confirms: libexec not copied"
cmp -s "$KG/schema-init" "$R/usr/bin/schema-init" || fail "bootok_confirms: kg schema-init differs"
[ ! -e "$KG.new" ] && [ ! -e "$KG.old" ] || fail "bootok_confirms: leftover dirs"
grep -q '^pi-bootok: boot confirmed after [0-9]*s (target=current)$' "$T/log" || fail "bootok_confirms: log: $(cat "$T/log")"
echo "ok bootok_confirms"

# bootok_names_blocker (also: DORMANT blocks, sshd missing blocks)
reset
printf 'service.sshd.state=FUNDAMENTAL\nservice.wpa-supplicant.state=RECOVERY\nservice.dead.state=DORMANT\n' > "$T/kv"
env_run sh "$BOOTOK" > "$T/log" 2>&1 &
pid=$!
sleep 0.5
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ ! -e "$MARK" ] || fail "bootok_names_blocker: marker written"
grep -qx 'pi-bootok: waiting on wpa-supplicant=RECOVERY' "$T/log" || fail "bootok_names_blocker: log: $(cat "$T/log")"
grep -qx 'pi-bootok: waiting on dead=DORMANT' "$T/log" || fail "bootok_names_blocker: dormant not named"
[ "$(grep -c 'waiting on wpa' "$T/log")" = 1 ] || fail "bootok_names_blocker: repeated inside 60s"
reset
printf 'service.net.state=FUNDAMENTAL\n' > "$T/kv"
env_run sh "$BOOTOK" > "$T/log" 2>&1 &
pid=$!
sleep 0.5
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ ! -e "$MARK" ] || fail "bootok_names_blocker: confirmed without sshd"
grep -qx 'pi-bootok: waiting on sshd=MISSING' "$T/log" || fail "bootok_names_blocker: sshd=MISSING not logged"
echo "ok bootok_names_blocker"

# bootok_no_promote_on_kg
reset known-good; good_kv
mkdir -p "$KG"; echo old > "$KG/schema-init"
ino=$(ls -di "$KG"); mt=$(ls -ld --time-style=full-iso "$KG")
sleep 1
env_run sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_no_promote_on_kg: exit"
[ -f "$MARK" ] || fail "bootok_no_promote_on_kg: no marker"
[ "$(ls -di "$KG")" = "$ino" ] || fail "bootok_no_promote_on_kg: inode changed"
[ "$(ls -ld --time-style=full-iso "$KG")" = "$mt" ] || fail "bootok_no_promote_on_kg: mtime changed"
[ "$(cat "$KG/schema-init")" = old ] || fail "bootok_no_promote_on_kg: content changed"
[ "$(cat "$STATE")" = "attempts=0
last_target=known-good" ] || fail "bootok_no_promote_on_kg: state: $(cat "$STATE")"
echo "ok bootok_no_promote_on_kg"

# malformed state falls back to current
reset; good_kv; echo junk > "$STATE"
env_run sh "$BOOTOK" > "$T/log" 2>&1 || fail "malformed: exit"
[ "$(cat "$STATE")" = "attempts=0
last_target=current" ] || fail "malformed: state"
echo "ok bootok_malformed_state"

# deadline_reboots
reset
SCHEMA_PI_ROOT=$R SCHEMA_CTL=$T/ctl SCHEMA_PI_DEADLINE=1 sh "$DEADLINE" > "$T/log" 2>&1 &
pid=$!
sleep 2
alive=0; kill -0 "$pid" 2>/dev/null && alive=1
pkill -P "$pid" 2>/dev/null; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ "$(cat "$T/reboots" 2>/dev/null)" = reboot ] || fail "deadline_reboots: no single reboot"
[ "$alive" = 1 ] || fail "deadline_reboots: exited after acting"
grep -qx 'pi-deadline: no boot-ok after 1s, rebooting' "$T/log" || fail "deadline_reboots: log: $(cat "$T/log")"
echo "ok deadline_reboots"

# deadline_quiet
reset; mkdir -p "$R/run/schema-init"; : > "$MARK"
SCHEMA_PI_ROOT=$R SCHEMA_CTL=$T/ctl SCHEMA_PI_DEADLINE=1 sh "$DEADLINE" > "$T/log" 2>&1 &
pid=$!
sleep 2
alive=0; kill -0 "$pid" 2>/dev/null && alive=1
pkill -P "$pid" 2>/dev/null; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ ! -e "$T/reboots" ] || fail "deadline_quiet: rebooted"
[ "$alive" = 1 ] || fail "deadline_quiet: exited"
! grep -q pi-deadline "$T/log" || fail "deadline_quiet: not quiet: $(cat "$T/log")"
echo "ok deadline_quiet"
