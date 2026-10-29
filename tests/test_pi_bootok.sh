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
    mkdir -p "$R/proc"
    echo 'console=tty1 panic=10 init=/usr/sbin/schema-init-pi' > "$R/proc/cmdline"
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
SCHEMA_PI_ROOT=$R SCHEMA_CTL=$T/ctl SCHEMA_PI_POLL=0.1 sh "$BOOTOK" > "$T/log" 2>&1 &
pid=$!
sleep 0.5
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ ! -e "$MARK" ] || fail "bootok_names_blocker: marker written"
grep -qx 'pi-bootok: waiting on wpa-supplicant=RECOVERY' "$T/log" || fail "bootok_names_blocker: log: $(cat "$T/log")"
grep -qx 'pi-bootok: waiting on dead=DORMANT' "$T/log" || fail "bootok_names_blocker: dormant not named"
[ "$(grep -c 'waiting on wpa' "$T/log")" = 1 ] || fail "bootok_names_blocker: repeated inside 60s"
reset
printf 'service.net.state=FUNDAMENTAL\n' > "$T/kv"
SCHEMA_PI_ROOT=$R SCHEMA_CTL=$T/ctl SCHEMA_PI_POLL=0.1 sh "$BOOTOK" > "$T/log" 2>&1 &
pid=$!
sleep 0.5
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ ! -e "$MARK" ] || fail "bootok_names_blocker: confirmed without sshd"
grep -qx 'pi-bootok: waiting on sshd=MISSING' "$T/log" || fail "bootok_names_blocker: sshd=MISSING not logged"
echo "ok bootok_names_blocker"

# list kv: avahi excised, everything else up
list_kv() {
    printf 'service.ssh.state=FUNDAMENTAL\nservice.avahi-daemon.state=EXCISED\nservice.pi-bootok.state=NEW_PROCESS\n' > "$T/kv"
}
env_to() { SCHEMA_PI_ROOT=$R SCHEMA_CTL=$T/ctl SCHEMA_PI_POLL=0.1 timeout 10 "$@"; }
waits_on() {
    # not via env_run: backgrounding a function leaves the sh orphaned past the kill, and it would confirm in the next case
    SCHEMA_PI_ROOT=$R SCHEMA_CTL=$T/ctl SCHEMA_PI_POLL=0.1 sh "$BOOTOK" > "$T/log" 2>&1 &
    pid=$!
    sleep 0.5
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    [ ! -e "$MARK" ]
}

# bootok_list_confirms: only listed services can block
reset; list_kv
printf '# hand\n\nssh\r\n' > "$R/etc/schema-init/pi-boot-required"
env_to sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_list_confirms: exit: $(cat "$T/log")"
[ -f "$MARK" ] || fail "bootok_list_confirms: no marker"
echo "ok bootok_list_confirms"

# bootok_list_waits: a listed service that is down blocks; unlisted missing does not
reset; printf 'service.ssh.state=DORMANT\nservice.avahi-daemon.state=FUNDAMENTAL\n' > "$T/kv"
printf 'ssh\n' > "$R/etc/schema-init/pi-boot-required"
waits_on || fail "bootok_list_waits: confirmed"
grep -qx 'pi-bootok: waiting on ssh=DORMANT' "$T/log" || fail "bootok_list_waits: log: $(cat "$T/log")"
reset; list_kv
printf 'ssh\nnetworkmanager\n' > "$R/etc/schema-init/pi-boot-required"
waits_on || fail "bootok_list_missing: confirmed"
grep -qx 'pi-bootok: waiting on networkmanager=MISSING' "$T/log" || fail "bootok_list_missing: log: $(cat "$T/log")"
echo "ok bootok_list_waits"

# bootok_no_list_old_rule
reset; list_kv
printf 'service.sshd.state=FUNDAMENTAL\nservice.avahi-daemon.state=EXCISED\n' > "$T/kv"
waits_on || fail "bootok_no_list_old_rule: confirmed"
grep -qx 'pi-bootok: waiting on avahi-daemon=EXCISED' "$T/log" || fail "bootok_no_list_old_rule: log: $(cat "$T/log")"
echo "ok bootok_no_list_old_rule"

# bootok_list_ssh_either: sshd listed, only ssh loaded
reset; list_kv
printf 'sshd\n' > "$R/etc/schema-init/pi-boot-required"
env_to sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_list_ssh_either: exit"
[ -f "$MARK" ] || fail "bootok_list_ssh_either: no marker"
reset; printf 'service.net.state=FUNDAMENTAL\n' > "$T/kv"
printf 'ssh\n' > "$R/etc/schema-init/pi-boot-required"
waits_on || fail "bootok_list_ssh_either: confirmed without ssh"
grep -qx 'pi-bootok: waiting on sshd=MISSING' "$T/log" || fail "bootok_list_ssh_either: log: $(cat "$T/log")"
echo "ok bootok_list_ssh_either"

# bootok_list_self_and_empty: the deadline/bootok names never block; a list with no names is no list
reset; list_kv
printf 'ssh\npi-bootok\npi-deadline\n' > "$R/etc/schema-init/pi-boot-required"
env_to sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_list_self: exit: $(cat "$T/log")"
[ -f "$MARK" ] || fail "bootok_list_self: no marker"
reset; list_kv
printf '# nothing\n\n' > "$R/etc/schema-init/pi-boot-required"
waits_on || fail "bootok_list_empty: confirmed"
grep -qx 'pi-bootok: waiting on avahi-daemon=EXCISED' "$T/log" || fail "bootok_list_empty: old rule not applied: $(cat "$T/log")"
echo "ok bootok_list_self_and_empty"

# bootok_settled: the engine counts SETTLED as ready (a recovered daemon), so must boot OK
reset; printf 'service.sshd.state=SETTLED\nservice.net.state=SETTLED\n' > "$T/kv"
env_to sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_settled: no list: $(cat "$T/log")"
[ -f "$MARK" ] || fail "bootok_settled: no list: no marker"
reset; printf 'service.ssh.state=SETTLED\nservice.net.state=SETTLED\nservice.avahi-daemon.state=EXCISED\n' > "$T/kv"
printf 'ssh\nnet\n' > "$R/etc/schema-init/pi-boot-required"
env_to sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_settled: list: $(cat "$T/log")"
[ -f "$MARK" ] || fail "bootok_settled: list: no marker"
echo "ok bootok_settled"

# bootok_blockers: prints the blockers once, exits 0, touches nothing
reset; printf 'service.ssh.state=DORMANT\nservice.net.state=SETTLED\nservice.avahi-daemon.state=EXCISED\nservice.pi-bootok.state=NEW_PROCESS\n' > "$T/kv"
printf 'ssh\nnet\nghost\n' > "$R/etc/schema-init/pi-boot-required"
cp "$STATE" "$T/state0"
env_to sh "$BOOTOK" --blockers > "$T/log" 2>&1 || fail "bootok_blockers: exit"
[ "$(cat "$T/log")" = "ghost=MISSING
ssh=DORMANT" ] || fail "bootok_blockers: $(cat "$T/log")"
[ ! -e "$MARK" ] && cmp -s "$STATE" "$T/state0" && [ ! -e "$KG" ] || fail "bootok_blockers: wrote state"
env_to sh "$BOOTOK" --blockers --all > "$T/log" 2>&1 || fail "bootok_blockers --all: exit"
[ "$(cat "$T/log")" = "avahi-daemon=EXCISED
ghost=MISSING
ssh=DORMANT" ] || fail "bootok_blockers --all: $(cat "$T/log")"
: > "$T/kv"
env_to sh "$BOOTOK" --blockers --all > "$T/log" 2>&1 || fail "bootok_blockers no engine: exit"
[ ! -s "$T/log" ] || fail "bootok_blockers --all: printed with no engine: $(cat "$T/log")"
echo "ok bootok_blockers"

# bootok_promote_copies_list
reset; list_kv
printf '# hand\nssh\n' > "$R/etc/schema-init/pi-boot-required"
env_to sh "$BOOTOK" --promote > "$T/log" 2>&1 || fail "bootok_promote_copies_list: exit"
cmp -s "$KG/pi-boot-required" "$R/etc/schema-init/pi-boot-required" || fail "bootok_promote_copies_list: differs"
reset
env_to sh "$BOOTOK" --promote > "$T/log" 2>&1 || fail "bootok_promote_copies_list: exit2"
[ ! -e "$KG/pi-boot-required" ] || fail "bootok_promote_copies_list: copied a missing list"
echo "ok bootok_promote_copies_list"

# bootok_kg_uses_kg_list: on a known-good boot the /etc list is ignored
reset known-good; list_kv
printf 'ssh\navahi-daemon\n' > "$R/etc/schema-init/pi-boot-required"
mkdir -p "$KG"; printf 'ssh\n' > "$KG/pi-boot-required"
env_to sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_kg_uses_kg_list: exit: $(cat "$T/log")"
[ -f "$MARK" ] || fail "bootok_kg_uses_kg_list: no marker"
reset known-good; list_kv
printf 'ssh\n' > "$R/etc/schema-init/pi-boot-required"
mkdir -p "$KG"
waits_on || fail "bootok_kg_uses_kg_list: old known-good without a list must keep today's rule"
echo "ok bootok_kg_uses_kg_list"

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

# bootok_stale_new
reset; good_kv
mkdir -p "$KG.new/services"; echo x > "$KG.new/services/old.svc"
env_run sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_stale_new: exit"
[ ! -e "$KG/services/old.svc" ] || fail "bootok_stale_new: stale file survived"
[ -f "$KG/services/sshd.svc" ] || fail "bootok_stale_new: not populated"
echo "ok bootok_stale_new"

# bootok_restore_old (current and known-good targets)
for tg in current known-good; do
    reset "$tg"; good_kv
    mkdir -p "$KG.old"; echo kept > "$KG.old/schema-init"
    env_run sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_restore_old($tg): exit"
    [ -d "$KG" ] && [ -f "$KG/schema-init" ] || fail "bootok_restore_old($tg): known-good missing"
    [ ! -e "$KG.old" ] || fail "bootok_restore_old($tg): .old remains"
    if [ "$tg" = known-good ]; then
        [ "$(cat "$KG/schema-init")" = kept ] || fail "bootok_restore_old: not restored"
    else
        [ -f "$KG/services/sshd.svc" ] || fail "bootok_restore_old: not populated"
    fi
done
echo "ok bootok_restore_old"

# bootok_promote_only
reset
env_run sh "$BOOTOK" --promote > "$T/log" 2>&1 || fail "bootok_promote_only: exit"
cmp -s "$KG/schema-init" "$R/usr/bin/schema-init" || fail "bootok_promote_only: kg not populated"
[ ! -e "$MARK" ] || fail "bootok_promote_only: marker written"
[ "$(cat "$STATE")" = "attempts=2
last_target=current" ] || fail "bootok_promote_only: state touched"
rm "$R/usr/bin/schema-ctl"
env_run sh "$BOOTOK" --promote > "$T/log" 2>&1 && fail "bootok_promote_only: should fail"
cmp -s "$KG/schema-init" "$R/usr/bin/schema-init" || fail "bootok_promote_only: good set lost"
echo "ok bootok_promote_only"

# bootok_svc_no_timeout: the default 90 s oneshot timeout kills a slow Zero W boot before it confirms
grep -qx 'start_timeout_sec=0' services/pi-bootok.svc.example || fail "bootok_svc_no_timeout: pi-bootok.svc.example keeps the oneshot timeout"
echo "ok bootok_svc_no_timeout"

# bootok_kg_runs_kg: the known-good svc files must run the known-good scripts
reset
printf 'name=pi-bootok\nexec=/usr/libexec/schema-init/schema-pi-bootok\noneshot=1\n' > "$R/etc/schema-init/services/pi-bootok.svc"
printf 'name=pi-deadline\nexec=/usr/libexec/schema-init/schema-pi-deadline\noneshot=0\n' > "$R/etc/schema-init/services/pi-deadline.svc"
env_run sh "$BOOTOK" --promote > "$T/log" 2>&1 || fail "bootok_kg_runs_kg: exit: $(cat "$T/log")"
[ "$(grep '^exec=' "$KG/services/pi-bootok.svc")" = exec=/var/lib/schema-init/known-good/schema-pi-bootok ] ||
    fail "bootok_kg_runs_kg: pi-bootok.svc: $(cat "$KG/services/pi-bootok.svc")"
[ "$(grep '^exec=' "$KG/services/pi-deadline.svc")" = exec=/var/lib/schema-init/known-good/schema-pi-deadline ] ||
    fail "bootok_kg_runs_kg: pi-deadline.svc: $(cat "$KG/services/pi-deadline.svc")"
grep -qx oneshot=1 "$KG/services/pi-bootok.svc" || fail "bootok_kg_runs_kg: other lines lost"
grep -q '^exec=/usr/libexec/' "$R/etc/schema-init/services/pi-bootok.svc" || fail "bootok_kg_runs_kg: live svc rewritten"
echo "ok bootok_kg_runs_kg"

# ctl_beside_script: a script run from known-good must use the known-good schema-ctl
reset; good_kv
mkdir -p "$T/kgbin"
cp "$BOOTOK" "$DEADLINE" "$T/kgbin/"
cp "$T/ctl" "$T/kgbin/schema-ctl"
# the host may have a real schema-ctl on PATH: a fallback to it must never reach a real reboot
mkdir -p "$T/pathctl"
printf '#!/bin/sh\necho "path-ctl $*" >> "%s/pathctl.log"\nexit 1\n' "$T" > "$T/pathctl/schema-ctl"
chmod +x "$T/pathctl/schema-ctl"
SCHEMA_PI_ROOT=$R SCHEMA_PI_POLL=0.1 PATH=$T/pathctl:$PATH timeout 5 sh "$T/kgbin/schema-pi-bootok" > "$T/log" 2>&1 ||
    fail "ctl_beside_script: bootok: $(cat "$T/log")"
[ -f "$MARK" ] || fail "ctl_beside_script: bootok did not use the beside ctl"
rm -f "$MARK"
SCHEMA_PI_ROOT=$R SCHEMA_PI_DEADLINE=1 PATH=$T/pathctl:$PATH sh "$T/kgbin/schema-pi-deadline" > "$T/log" 2>&1 &
pid=$!
sleep 2
pkill -P "$pid" 2>/dev/null; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ "$(cat "$T/reboots" 2>/dev/null)" = reboot ] || fail "ctl_beside_script: deadline did not use the beside ctl: $(cat "$T/log")"
echo "ok ctl_beside_script"

# bootok_swap_fails: a failed .new -> known-good rename must put the old set back
reset
env_run sh "$BOOTOK" --promote > "$T/log" 2>&1 || fail "bootok_swap_fails: seed"
echo bin-init-2 > "$R/usr/bin/schema-init"
mkdir -p "$T/mvfail"
printf '#!/bin/sh\ncase "$1" in *.new) exit 1 ;; esac\nexec %s "$@"\n' "$(command -v mv)" > "$T/mvfail/mv"
chmod +x "$T/mvfail/mv"
PATH=$T/mvfail:$PATH env_run sh "$BOOTOK" --promote > "$T/log" 2>&1 && fail "bootok_swap_fails: reported success"
[ "$(cat "$KG/schema-init" 2>/dev/null)" = bin-init ] || fail "bootok_swap_fails: old set not restored"
[ ! -e "$KG.new" ] && [ ! -e "$KG.old" ] || fail "bootok_swap_fails: leftover dirs"
grep -q 'known-good promotion failed' "$T/log" || fail "bootok_swap_fails: not logged"
echo "ok bootok_swap_fails"

# deadline_retries_reboot: a failed reboot must be retried, not left to sleep forever
reset
cat > "$T/flaky" <<EOT
#!/bin/sh
echo try >> "$T/tries"
[ "\$(wc -l < "$T/tries")" -ge 3 ] || exit 1
echo reboot >> "$T/reboots"
EOT
chmod +x "$T/flaky"; rm -f "$T/tries"
SCHEMA_PI_ROOT=$R SCHEMA_CTL=$T/flaky SCHEMA_PI_DEADLINE=0.1 SCHEMA_PI_REBOOT_RETRY=0.2 sh "$DEADLINE" > "$T/log" 2>&1 &
pid=$!
sleep 2
pkill -P "$pid" 2>/dev/null; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ "$(wc -l < "$T/tries" 2>/dev/null)" = 3 ] || fail "deadline_retries_reboot: tries: $(cat "$T/tries" 2>/dev/null)"
[ "$(cat "$T/reboots" 2>/dev/null)" = reboot ] || fail "deadline_retries_reboot: no reboot"
[ "$(grep -c '^pi-deadline: reboot failed, retrying in 0.2s$' "$T/log")" = 2 ] || fail "deadline_retries_reboot: log: $(cat "$T/log")"
echo "ok deadline_retries_reboot"

# bootok_reason_not_state: a .reason= value that happens to contain .state= is not a service state
reset; good_kv
echo 'service.net.reason=dep.state=FAILED' >> "$T/kv"
env_run sh "$BOOTOK" > "$T/log" 2>&1 &
pid=$!
sleep 1
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ -f "$MARK" ] || fail "bootok_reason_not_state: blocked: $(cat "$T/log")"
echo "ok bootok_reason_not_state"

# bootok_snapshot_real_files: a symlinked svc must land in known-good as its contents, not a link
reset; good_kv
echo linked > "$T/linked.svc"
ln -s "$T/linked.svc" "$R/etc/schema-init/services/linked.svc"
env_run sh "$BOOTOK" > "$T/log" 2>&1 || fail "bootok_snapshot_real_files: exit"
[ -f "$KG/services/linked.svc" ] && [ ! -L "$KG/services/linked.svc" ] || fail "bootok_snapshot_real_files: still a link"
[ "$(cat "$KG/services/linked.svc")" = linked ] || fail "bootok_snapshot_real_files: contents"
echo "ok bootok_snapshot_real_files"

# deadline_needs_wrapper: booted without the wrapper (systemd, or schema-init direct) there is no fallback to reboot into
reset
echo 'console=tty1 init=/sbin/schema-init' > "$R/proc/cmdline"
SCHEMA_PI_ROOT=$R SCHEMA_CTL=$T/ctl SCHEMA_PI_DEADLINE=0.1 sh "$DEADLINE" > "$T/log" 2>&1 &
pid=$!
sleep 1
alive=0; kill -0 "$pid" 2>/dev/null && alive=1
pkill -P "$pid" 2>/dev/null; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ ! -e "$T/reboots" ] || fail "deadline_needs_wrapper: rebooted"
[ "$alive" = 1 ] || fail "deadline_needs_wrapper: exited"
echo "ok deadline_needs_wrapper"
