#!/bin/bash
# schema-init VM boot-test harness.
# Boots the freshly-built schema-init as init=/sbin/schema-init inside QEMU,
# using the SAME kernel as the hardware, with only the 3 test svcs loaded.
# Verifies PR #7 (timers) + PR #8 (start_timeout) without a hardware reboot.
set -euo pipefail

# Resolve alongside the script rather than a fixed ~/schema-livetest, so this
# runs both from a repo checkout and from the historical harness directory
# (whose entries are symlinks back here). OUT holds the kept serial logs.
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE}"

REPO="${REPO:-$HOME/projects/schema-init}"
KERNEL="${KERNEL:-/lib/modules/$(uname -r)/vmlinuz}"
[ -r "$KERNEL" ] || KERNEL="/boot/vmlinuz-$(uname -r)"
BB="/usr/sbin/busybox"
WORK="$(mktemp -d /var/tmp/schema-vmtest.XXXXXX)"
ROOT="$WORK/root"
SERIAL="$WORK/serial.log"
TIMEOUT="${TIMEOUT:-170}"
REBUILD="${REBUILD:-1}"

trap 'rm -rf "$WORK"' EXIT
echo ">> workdir: $WORK"

# 1. Build current branch (always current with the checked-out tree).
if [ "$REBUILD" = 1 ]; then
  echo ">> building schema-init ($(git -C "$REPO" rev-parse --abbrev-ref HEAD))"
  make -C "$REPO" >/dev/null 2>&1 || { echo "BUILD FAILED"; make -C "$REPO"; exit 1; }
fi
BIN="$REPO/schema-init"
[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 1; }

# Static privdrop helper (initramfs has no libc -> must be -static). Exercises
# chronyd's real self-privdrop so the cap set is validated, not just asserted.
MOUNTNS="$WORK/test_mountns"
cc -static -O2 -std=c11 -D_GNU_SOURCE -o "$MOUNTNS" "$HERE/test_mountns.c" \
  || { echo "MOUNTNS HELPER BUILD FAILED"; exit 1; }
SCTL="$WORK/schema-ctl"
cc -static -O2 -std=c99 -D_GNU_SOURCE -I"$REPO" -o "$SCTL" "$REPO/schema-ctl.c" \
  || { echo "SCHEMA-CTL STATIC BUILD FAILED"; exit 1; }
PRIVDROP="$WORK/test_privdrop"
cc -static -O2 -std=c11 -D_GNU_SOURCE -o "$PRIVDROP" "$HERE/test_privdrop.c" \
  || { echo "PRIVDROP HELPER BUILD FAILED"; cc -static -std=c11 -D_GNU_SOURCE -o "$PRIVDROP" "$HERE/test_privdrop.c"; exit 1; }
NOTIFIER="$WORK/notifier"
cc -static -O2 -D_GNU_SOURCE -o "$NOTIFIER" "$HERE/notifycap/notifier.c" || { echo "NOTIFIER BUILD FAILED"; exit 1; }

# Pi boot fallback: schema-init-pi is /init on a real ext4 root, booted four
# times from one disk so state, known-good and logs carry over as on the SD card.
if [ "${PI_WRAPPER:-0}" = 1 ]; then
  for t in fakeroot debugfs dumpe2fs e2fsck; do
    command -v "$t" >/dev/null || { echo "PI_WRAPPER needs $t (e2fsprogs, fakeroot) on PATH"; exit 1; }
  done
  if [ "$REBUILD" = 1 ]; then
    make -C "$REPO" schema-init-pi >/dev/null 2>&1 || { echo "PI WRAPPER BUILD FAILED"; make -C "$REPO" schema-init-pi; exit 1; }
  fi
  PIW="$REPO/schema-init-pi"
  [ -x "$PIW" ] || { echo "no wrapper at $PIW"; exit 1; }
  PR="$WORK/piroot"
  IMG="$WORK/pi.img"
  mkdir -p "$PR"/{bin,dev,proc,sys,run,tmp,lib/systemd,usr/bin,usr/libexec/schema-init,etc/schema-init/services,var/lib/schema-init,var/log/schema-init}
  cp "$PIW" "$PR/init"
  cp "$BIN" "$PR/usr/bin/schema-init"
  cp "$SCTL" "$PR/usr/bin/schema-ctl"
  cp "$BB" "$PR/bin/busybox"
  for a in sh cat ls sleep touch poweroff mount mkdir echo grep head tail kill readlink tr cut awk sed date sync mv cp rm; do
    ln -sf /bin/busybox "$PR/bin/$a"
  done
  # pi-bootok promotes both scripts into known-good, so both must exist
  cp "$REPO/scripts/schema-pi-bootok" "$REPO/scripts/schema-pi-deadline" "$PR/usr/libexec/schema-init/"
  cp "$REPO/services/pi-bootok.svc.example" "$PR/etc/schema-init/services/pi-bootok.svc"
  # pi-bootok will not confirm a boot without an sshd service up
  cat > "$PR/etc/schema-init/services/sshd.svc" <<'EOT'
name=sshd
exec=/bin/sh
args=-c
args=touch /run/sshd.ready; exec sleep 100000
ready_path=/run/sshd.ready
needs_root=1
EOT
  # Long-running and ready at once: pi-bootok waits on every service, this one too.
  cat > "$PR/usr/bin/pi-report.sh" <<'EOT'
#!/bin/sh
touch /run/pi-report.ready
L=/var/log/schema-init
n=0
until grep -q 'boot confirmed' $L/pi-bootok.log 2>/dev/null || [ $n -ge 120 ]; do
    n=$((n + 1)); sleep 1
done
sleep 2
exec >/dev/console 2>&1   # service stdout is captured to a logfile; force serial
echo "===== PI-REPORT ====="
echo "PI-EXE: $(readlink /proc/1/exe)"
echo "PI-ENV: [$(tr '\0' '\n' < /proc/1/environ | grep '^SCHEMA_SVC_DIR=')]"
echo "PI-ROOT: $(grep ' / ext4 ' /proc/mounts | cut -d' ' -f4 | cut -d, -f1)"
echo "PI-CMDLINE: $(cat /proc/cmdline)"
echo "PI-SVCS: $(schema-ctl status --kv | grep '\.state=' | tr '\n' ' ')"
while read -r l; do echo "BOOTOK| $l"; done < $L/pi-bootok.log
grep -E ' (spawn|oneshot-done) ' $L/boot.log | while read -r l; do echo "RAIL| $l"; done
while read -r l; do echo "FALLBACK| $l"; done < $L/fallback.log
# a regular file on PID 1's fds pins / writable and fails the shutdown remount-ro
for f in 0 1 2; do echo "PI-FD$f: $(readlink /proc/1/fd/$f)"; done
echo "===== PI-REPORT-END ====="
( sleep 45; echo "SHUTDOWN-WEDGED: forcing poweroff"; poweroff -f ) &
kill -INT 1
while :; do sleep 1; done
EOT
  chmod +x "$PR/usr/bin/pi-report.sh"
  cat > "$PR/etc/schema-init/services/pi-report.svc" <<'EOT'
name=pi-report
exec=/usr/bin/pi-report.sh
ready_path=/run/pi-report.ready
needs_root=1
EOT
  # The wrapper reopened the console before exec, so plain stdout must reach serial.
  cat > "$PR/lib/systemd/systemd" <<'EOT'
#!/bin/sh
echo "PI-SYSTEMD-REACHED argv0=$0"
sync
poweroff -f
EOT
  chmod +x "$PR/lib/systemd/systemd"
  # fakeroot: the engine refuses .svc files not owned by root
  fakeroot -- mkfs.ext4 -q -F -d "$PR" "$IMG" 128M >/dev/null || { echo "PI IMAGE BUILD FAILED"; exit 1; }
  # debugfs edits the image between boots without root or a loop mount. It
  # bypasses the journal, so replay it first or the next mount undoes the edit.
  pi_replay() { e2fsck -y -E journal_only "$IMG" >/dev/null 2>&1 || true; }
  pi_fs() { pi_replay; printf '%s\n' "$@" > "$WORK/pi-fs.cmd"; debugfs -w -f "$WORK/pi-fs.cmd" "$IMG" >/dev/null 2>&1; }
  pi_seed() {
    printf 'attempts=%s\nlast_target=%s\n' "$1" "$2" > "$WORK/pi-seed"
    pi_fs "cd /var/lib/schema-init" "rm pi-boot" "write $WORK/pi-seed pi-boot"
  }
  pi_boot() {
    local s="$WORK/pi-serial-$1.log"
    # both logs append across boots; a stale line must not satisfy this boot
    pi_fs "rm /var/log/schema-init/pi-bootok.log" "rm /var/log/schema-init/fallback.log"
    echo ">> PI boot $1 (root $2), timeout ${TIMEOUT}s"
    timeout "$TIMEOUT" qemu-system-x86_64 \
      -enable-kvm -m 512 -smp 2 -no-reboot -nographic -nic none \
      -kernel "$KERNEL" -drive "file=$IMG,format=raw,if=virtio" \
      -append "console=ttyS0 root=/dev/vda rootfstype=ext4 $2 init=/init panic=1 loglevel=4" \
      < /dev/null > "$s" 2>&1 || true
    { echo "===== PI BOOT $1 (root $2) ====="; cat "$s"; } >> "$OUT/last-vmtest-serial.log"
    # before the replay, which would clear it: a clean ro remount leaves no journal to recover
    dumpe2fs -h "$IMG" 2>/dev/null | grep '^Filesystem features:' > "$WORK/pi-fsflags-$1"
    pi_replay
    debugfs -R "cat /var/lib/schema-init/pi-boot" "$IMG" > "$WORK/pi-state-$1" 2>/dev/null
    debugfs -R "cat /var/log/schema-init/fallback.log" "$IMG" > "$WORK/pi-fallback-$1" 2>/dev/null
  }
  : > "$OUT/last-vmtest-serial.log"
  pi_seed 0 current
  pi_boot 1 rw
  debugfs -R "dump /var/lib/schema-init/known-good/schema-init $WORK/pi-kg-1" "$IMG" >/dev/null 2>&1
  pi_seed 2 current
  # a differing known-good that still runs: trailing bytes past the ELF are ignored
  cp "$BIN" "$WORK/pi-kg-bin"; printf 'known-good' >> "$WORK/pi-kg-bin"
  # only the known-good svc dir has this, so seeing it proves the engine read that dir
  printf 'name=kg-marker\nexec=/bin/sh\nargs=-c\nargs=touch /run/kg.ready; exec sleep 100000\nready_path=/run/kg.ready\nneeds_root=1\n' > "$WORK/kg-marker.svc"
  chmod 644 "$WORK/kg-marker.svc"
  pi_fs "cd /var/lib/schema-init/known-good" "rm schema-init" "write $WORK/pi-kg-bin schema-init" \
        "cd services" "write $WORK/kg-marker.svc kg-marker.svc" "sif kg-marker.svc uid 0" "sif kg-marker.svc gid 0"
  pi_boot 2 rw
  pi_seed 3 known-good
  pi_boot 3 rw
  # no seed: boot 3's reset to attempts=0 must carry over on the disk
  pi_boot 4 ro
  # Both engine execs fail after to_boot_log(), so the stub's stdout can only
  # reach serial if the wrapper reopened the console. Last: the disk stays broken.
  pi_seed 0 current
  pi_fs "sif /usr/bin/schema-init mode 0100644" "rm /var/lib/schema-init/known-good/schema-init"
  pi_boot 5 rw

  pass=1
  pi_has() { grep -Eq "$2" "$WORK/pi-serial-$1.log"; }
  pi_miss() { echo "  MISS: $1"; pass=0; }
  pi_state() { [ "$(tr '\n' ' ' < "$WORK/pi-state-$1")" = "$2 " ]; }
  pi_has 1 "PI-EXE: /usr/bin/schema-init" || pi_miss "PI-current: engine was not /usr/bin/schema-init"
  pi_has 1 "PI-ENV: \[\]"                  || pi_miss "PI-current: SCHEMA_SVC_DIR set on a current boot"
  pi_has 1 "RAIL\| .*sshd +spawn"          || pi_miss "PI-current: rail did not run"
  pi_has 1 "BOOTOK\| pi-bootok: boot confirmed .*\(target=current\)" || pi_miss "PI-current: pi-bootok did not confirm"
  pi_state 1 "attempts=0 last_target=current" || pi_miss "PI-current: state after boot ($(tr '\n' ' ' < "$WORK/pi-state-1"))"
  cmp -s "$WORK/pi-kg-1" "$BIN"           || pi_miss "PI-current: known-good not promoted from the current engine"
  pi_has 2 "PI-EXE: /var/lib/schema-init/known-good/schema-init" || pi_miss "PI-knowngood: engine was not the known-good copy"
  pi_has 2 "PI-ENV: \[SCHEMA_SVC_DIR=/var/lib/schema-init/known-good/services\]" || pi_miss "PI-knowngood: svc dir not handed over"
  pi_has 2 "PI-SVCS: .*service\.kg-marker\.state=" || pi_miss "PI-knowngood: engine did not load the known-good svc dir"
  pi_has 2 "RAIL\| .*sshd +spawn"          || pi_miss "PI-knowngood: rail did not run"
  pi_has 2 "BOOTOK\| pi-bootok: boot confirmed .*\(target=known-good\)" || pi_miss "PI-knowngood: pi-bootok did not confirm"
  grep -Eq "^[0-9]+ fallback=known-good attempts=2$" "$WORK/pi-fallback-2" || pi_miss "PI-knowngood: fallback.log"
  pi_has 3 "PI-SYSTEMD-REACHED argv0=/lib/systemd/systemd" || pi_miss "PI-systemd: systemd stub never reached the console"
  pi_has 3 "PI-EXE:"                       && pi_miss "PI-systemd: an engine ran on the systemd boot"
  pi_state 3 "attempts=0 last_target=systemd" || pi_miss "PI-systemd: state after boot ($(tr '\n' ' ' < "$WORK/pi-state-3"))"
  grep -Eq "^[0-9]+ fallback=systemd attempts=3$" "$WORK/pi-fallback-3" || pi_miss "PI-systemd: fallback.log"
  pi_has 5 "PI-SYSTEMD-REACHED argv0=/lib/systemd/systemd" || pi_miss "PI-reopen: stub output stuck in boot.log, console not reopened"
  pi_has 5 "PI-EXE:"                       && pi_miss "PI-reopen: an engine ran with both engines unexecutable"
  pi_state 5 "attempts=1 last_target=current" || pi_miss "PI-reopen: state after boot ($(tr '\n' ' ' < "$WORK/pi-state-5"))"
  pi_has 4 "PI-CMDLINE: .* ro "            || pi_miss "PI-ro: kernel cmdline lacked ro"
  pi_has 4 "remount / rw failed"           && pi_miss "PI-ro: wrapper could not remount / rw"
  pi_has 4 "PI-ROOT: rw"                   || pi_miss "PI-ro: / still read-only under the engine"
  pi_has 4 "PI-EXE: /usr/bin/schema-init" || pi_miss "PI-ro: engine was not /usr/bin/schema-init"
  pi_has 4 "RAIL\| .*sshd +spawn"          || pi_miss "PI-ro: rail did not run"
  pi_has 4 "BOOTOK\| pi-bootok: boot confirmed .*\(target=current\)" || pi_miss "PI-ro: pi-bootok did not confirm"
  pi_state 4 "attempts=0 last_target=current" || pi_miss "PI-ro: state after boot ($(tr '\n' ' ' < "$WORK/pi-state-4"))"
  grep -q "SHUTDOWN-WEDGED" "$OUT/last-vmtest-serial.log" && pi_miss "a Pi boot wedged on shutdown, forced off"
  # boots 3 and 5 are the systemd stub's poweroff -f, not the engine's shutdown
  for b in 1 2 4; do
    pi_has $b "remount-ro / failed" && pi_miss "PI-shutdown: boot $b could not remount / ro ($(grep -a -o 'remount-ro / failed.*' "$WORK/pi-serial-$b.log" | tr -d '\r'); $(grep -a -o 'PI-FD[12]: .*' "$WORK/pi-serial-$b.log" | tr -d '\r' | tr '\n' ' '))"
    grep -q needs_recovery "$WORK/pi-fsflags-$b" && pi_miss "PI-shutdown: boot $b left the ext4 journal needing recovery"
  done
  if [ "$pass" = 1 ]; then
    echo ">> RESULT: PASS  (Pi wrapper: current, known-good, systemd fallback, console reopen, ro root)"
  else
    echo ">> RESULT: FAIL / INCONCLUSIVE — serial of every Pi boot: $OUT/last-vmtest-serial.log"
    trap - EXIT; echo "   workdir kept: $WORK"
  fi
  exit 0
fi

# 2. Assemble a minimal initramfs root.
mkdir -p "$ROOT"/{sbin,bin,usr/bin,etc/schema-init/services,proc,sys,dev,run,sys/fs/cgroup}
cp "$BIN" "$ROOT/sbin/schema-init"
ln -sf /sbin/schema-init "$ROOT/init"   # kernel initramfs entry point is /init
cp "$BB"  "$ROOT/bin/busybox"
for a in sh ls cat sleep touch poweroff mount mkdir echo grep head kill readlink tr cut wc; do
  ln -sf /bin/busybox "$ROOT/bin/$a"
done
ln -sf /bin/busybox "$ROOT/usr/bin/touch"   # test svcs call /usr/bin/touch
ln -sf /bin/busybox "$ROOT/bin/sleep"       # and /bin/sleep
cp "$PRIVDROP" "$ROOT/bin/test_privdrop"
cp "$NOTIFIER" "$ROOT/usr/bin/notifier"
cp "$MOUNTNS" "$ROOT/bin/test_mountns"
cp "$SCTL" "$ROOT/bin/schema-ctl"
ln -sf /bin/busybox "$ROOT/bin/rm"
ln -sf /bin/busybox "$ROOT/bin/chmod"

# ready_path service that deletes its readiness file on SIGTERM and lingers,
# like pipewire removing pipewire-0 on shutdown. schema-ctl restart used to see
# the file vanish before the reap, log readiness-lost, and park it DORMANT.
cat > "$ROOT/usr/bin/readypath.sh" <<'EOF'
#!/bin/sh
trap 'rm -f /run/test-rp.ready; sleep 2; exit 0' TERM
echo $$ > /run/test-rp.pid
touch /run/test-rp.ready
while :; do sleep 1; done
EOF
chmod +x "$ROOT/usr/bin/readypath.sh"
cat > "$ROOT/etc/schema-init/services/test-readypath.svc" <<'EOF'
name=test-readypath
exec=/usr/bin/readypath.sh
ready_path=/run/test-rp.ready
EOF
printf 'ENVF_V=from-file\n' > "$ROOT/etc/test-env"
cat > "$ROOT/etc/schema-init/services/test-envfile.svc" <<'EOF'
name=test-envfile
exec=/bin/sh
args=-c
args=echo "ENVF=$ENVF_V" > /run/test-envfile.out
env_file=/etc/test-env
oneshot=1
EOF
cat > "$ROOT/etc/schema-init/services/test-envopt.svc" <<'EOF'
name=test-envopt
exec=/bin/sh
args=-c
args=echo "ENVO=[$ENVF_V]" > /run/test-envopt.out
env_file=-/etc/test-env
oneshot=1
EOF
mkdir -p "$ROOT"/{boot/efi,home,root,tmp,var/tmp}

# Stale ready_path probe: run 1 leaves its marker behind and crashes; run 2
# removes the leftover during slow setup and only then writes a fresh one.
# The leftover must not promote run 2, or its removal reads as readiness-lost.
cat > "$ROOT/usr/bin/staleready.sh" <<'EOF'
#!/bin/sh
n=$(cat /run/test-stale.n 2>/dev/null || echo 0); n=$((n+1)); echo $n > /run/test-stale.n
if [ "$n" = 1 ]; then touch /run/test-stale.ready; sleep 3; exit 1; fi
sleep 4; rm -f /run/test-stale.ready; sleep 1; touch /run/test-stale.ready
while :; do sleep 1; done
EOF
chmod +x "$ROOT/usr/bin/staleready.sh"
cat > "$ROOT/etc/schema-init/services/test-stale.svc" <<'EOF'
name=test-stale
exec=/usr/bin/staleready.sh
ready_path=/run/test-stale.ready
stable_secs=600
EOF
# Same, but run 2 rewrites the leftover in place (O_TRUNC, same inode), as a
# pidfile writer does. That rewrite must still promote it.
cat > "$ROOT/usr/bin/staleinplace.sh" <<'EOF'
#!/bin/sh
n=$(cat /run/test-stale2.n 2>/dev/null || echo 0); n=$((n+1)); echo $n > /run/test-stale2.n
if [ "$n" = 1 ]; then echo 1 > /run/test-stale2.ready; sleep 3; exit 1; fi
sleep 4; echo 2 > /run/test-stale2.ready
while :; do sleep 1; done
EOF
chmod +x "$ROOT/usr/bin/staleinplace.sh"
cat > "$ROOT/etc/schema-init/services/test-stale2.svc" <<'EOF'
name=test-stale2
exec=/usr/bin/staleinplace.sh
ready_path=/run/test-stale2.ready
stable_secs=600
EOF

cp "$HERE/test-timer.svc"     "$ROOT/etc/schema-init/services/"
cp "$HERE/test-hang.svc"      "$ROOT/etc/schema-init/services/"
cp "$HERE/test-dependent.svc" "$ROOT/etc/schema-init/services/"

# A RUN-ONCE boot timer: on_boot_sec with no on_active_sec. Distinct from
# test-timer, which repeats. Reload used to re-run every one of these, because
# the terminal state (SVC_TIMER cleared) lived only on the live record while
# the shadow was parsed fresh from the .svc and inherited an already-expired
# timer_next. Each fire appends a line, so the count is the assertion.
cat > "$ROOT/usr/bin/runonce.sh" <<'EOF'
#!/bin/sh
echo fired >> /run/runonce.count
EOF
chmod +x "$ROOT/usr/bin/runonce.sh"
cat > "$ROOT/etc/schema-init/services/test-runonce.svc" <<'EOF'
name=test-runonce
exec=/usr/bin/runonce.sh
on_boot_sec=20
needs_root=1
EOF

# restart_count probe: a service that always dies, max_restarts=3. Only
# crash-driven respawns count, so it must retry exactly 3 times then go dormant.
cat > "$ROOT/usr/bin/crashloop.sh" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$ROOT/usr/bin/crashloop.sh"
# exec_stop / exec_stop_post probe: stop runs the stop command before
# SIGTERM and the post command after the service is gone
cat > "$ROOT/usr/bin/stopprobe.sh" <<'EOF'
#!/bin/sh
trap 'echo term >> /run/stopprobe.order; exit 0' TERM
while :; do sleep 1 & wait; done
EOF
chmod +x "$ROOT/usr/bin/stopprobe.sh"
cat > "$ROOT/etc/schema-init/services/test-stopcmd.svc" <<'EOF'
name=test-stopcmd
exec=/usr/bin/stopprobe.sh
exec_stop=/bin/sh -c "echo stop >> /run/stopprobe.order"
exec_stop_post=/bin/sh -c "echo post >> /run/stopprobe.order"
EOF

# main_pid_notify probes: a launcher hands over to a child via MAINPID=. The
# child that holds the notify sender keeps it alive, so PID 1 can attribute it.
mpsvc() {  # name script [extra .svc lines]
    printf '#!/bin/sh\n%s\n' "$2" > "$ROOT/usr/bin/$1.sh"; chmod +x "$ROOT/usr/bin/$1.sh"
    printf 'name=%s\nexec=/usr/bin/%s.sh\nnotify=1\nmain_pid_notify=1\n%b' "$1" "$1" "${3:-}" \
        > "$ROOT/etc/schema-init/services/$1.svc"
}
HAND='sh -c '"'"'exec /usr/bin/notifier "$(printf "MAINPID=%s\\nREADY=1" $$)" hold'"'"''
mpsvc mp-before "$HAND & echo \$! > /run/mp-before.pid; sleep 1; exit 0"
mpsvc mp-after "sh -c 'sleep 2; exec /usr/bin/notifier \"\$(printf \"MAINPID=%s\\\\nREADY=1\" \$\$)\" hold' & echo \$! > /run/mp-after.pid; exit 0"
mpsvc mp-fail "$HAND & echo \$! > /run/mp-fail.pid; sleep 1; exit 1" 'no_restart=1\n'
mpsvc mp-never 'exit 0' 'no_restart=1\n'
mpsvc mp-outside '/usr/bin/notifier "MAINPID=1" hold & sleep 1; exit 0' 'no_restart=1\n'
mpsvc mp-restart "sh -c '/usr/bin/notifier \"\$(printf \"MAINPID=%s\\\\nREADY=1\" \$\$)\" hold & sleep 3; exit 0' & sleep 1; exit 0" 'max_restarts=2\n'
mpsvc mp-norestart "sh -c '/usr/bin/notifier \"\$(printf \"MAINPID=%s\\\\nREADY=1\" \$\$)\" hold & sleep 3; exit 0' & sleep 1; exit 0" 'no_restart=1\n'
printf '#!/bin/sh\n%s & echo $! > /run/mp-nokey.pid; sleep 1; exit 0\n' "$HAND" > "$ROOT/usr/bin/mp-nokey.sh"; chmod +x "$ROOT/usr/bin/mp-nokey.sh"
# a plain notify daemon that is ready, then exits 0: an exit, not a handover (#434)
mpsvc mp-cleanexit '/usr/bin/notifier READY=1 hold & N=$!; sleep 2; kill $N; exit 0' 'no_restart=1\n'
# a launcher that never names a main pid, stopped while PID 1 waits for one (#434)
mpsvc mp-waitstop 'exit 0' 'no_restart=1\nexec_stop_post=/bin/sh -c "echo ran > /run/mp-waitstop.post"\n'
printf 'name=mp-nokey\nexec=/usr/bin/mp-nokey.sh\nnotify=1\nno_restart=1\n' > "$ROOT/etc/schema-init/services/mp-nokey.svc"

# nested cgroup probe (what podman --cgroups=split does): the service moves
# itself into <svc>/runtime and enables controllers on <svc>. Stop must remove
# the whole tree, and a restart must get a cgroup again.
cat > "$ROOT/usr/bin/cgnest.sh" <<'EOF'
#!/bin/sh
CG=/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup)
mkdir "$CG/runtime" "$CG/payload"
echo $$ > "$CG/runtime/cgroup.procs"
echo +memory > "$CG/cgroup.subtree_control"
echo "$CG" > "${1:-/run/cgnest.path}"
while :; do sleep 1; done
EOF
chmod +x "$ROOT/usr/bin/cgnest.sh"
cat > "$ROOT/etc/schema-init/services/test-cgnest.svc" <<'EOF'
name=test-cgnest
exec=/usr/bin/cgnest.sh
EOF
# a forking daemon that nests: its launcher's exit 0 is a handover, the
# tree must survive it (#435)
cat > "$ROOT/usr/bin/cgfork.sh" <<'EOF'
#!/bin/sh
/usr/bin/cgnest.sh /run/cgfork.path &
echo $! > /run/cgfork.pid
sleep 1
exit 0
EOF
chmod +x "$ROOT/usr/bin/cgfork.sh"
cat > "$ROOT/etc/schema-init/services/test-cgfork.svc" <<'EOF'
name=test-cgfork
exec=/usr/bin/cgfork.sh
pid_file=/run/cgfork.pid
no_restart=1
EOF

# verify_*_max probe: like podman, the service puts its payload in a child
# cgroup with a memory limit, then sends READY=1. verify-ok's limit lands,
# verify-bad's never does (the limit write is skipped).
cat > "$ROOT/usr/bin/verifycg.sh" <<'EOF'
#!/bin/sh
CG=/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup)
mkdir -p "$CG/runtime" "$CG/libpod-payload-x"
echo $$ > "$CG/runtime/cgroup.procs"
# the controller write fails while anything else is still in $CG: retry, and
# say so if it never lands, or verify-ok would really be missing its limit
i=0; until echo +memory > "$CG/cgroup.subtree_control" 2>/dev/null; do i=$((i+1)); [ $i -ge 50 ] && { echo "VERIFYCG +memory failed" > /dev/console; break; }; sleep 0.1; done
[ "$1" = ok ] && echo 67108864 > "$CG/libpod-payload-x/memory.max"
[ "$1" = noready ] && exec sleep 1000
sh -c 'echo $$ > "$0/cgroup.procs"; exec sleep 1000' "$CG/libpod-payload-x" &
sleep 1
exec /usr/bin/notifier READY=1 hold
EOF
chmod +x "$ROOT/usr/bin/verifycg.sh"
for v in ok bad noready; do
printf 'name=verify-%s\nexec=/usr/bin/verifycg.sh\nargs=%s\nnotify=1\nverify_memory_max=67108864\nno_restart=1\nstable_secs=1\n' "$v" "$v" \
    > "$ROOT/etc/schema-init/services/verify-$v.svc"
done

cat > "$ROOT/etc/schema-init/services/test-crash.svc" <<'EOF'
name=test-crash
exec=/usr/bin/crashloop.sh
max_restarts=3
EOF

# Dormant-dep probe: test-crashdep depends on test-crash (non-critical). Once
# test-crash goes dormant its dependent must start without it, not wait.
cat > "$ROOT/etc/schema-init/services/test-crashdep.svc" <<'EOF'
name=test-crashdep
exec=/usr/bin/slowloop.sh
dep=test-crash
EOF
cat > "$ROOT/etc/schema-init/services/test-hangdep.svc" <<'EOF'
name=test-hangdep
exec=/usr/bin/slowloop.sh
dep=test-hang
stable_secs=5
EOF

# Readiness-lost probe: run 1 goes ready, then deletes its ready_path and
# stays alive. That is readiness-lost; it must restart now, not go dormant.
cat > "$ROOT/usr/bin/rplost.sh" <<'EOF'
#!/bin/sh
n=$(cat /run/test-rplost.n 2>/dev/null || echo 0); n=$((n+1)); echo $n > /run/test-rplost.n
touch /run/test-rplost.ready
if [ "$n" = 1 ]; then sleep 3; rm -f /run/test-rplost.ready; fi
while :; do sleep 1; done
EOF
chmod +x "$ROOT/usr/bin/rplost.sh"
cat > "$ROOT/etc/schema-init/services/test-rplost.svc" <<'EOF'
name=test-rplost
exec=/usr/bin/rplost.sh
ready_path=/run/test-rplost.ready
EOF

# analyze probe: test-slow has no readiness signal, so the 10s stable timer
# promotes it; test-after waits on it. The chain to test-after must walk to
# test-slow and flag it as timer-gated.
cat > "$ROOT/usr/bin/slowloop.sh" <<'EOF'
#!/bin/sh
while true; do sleep 30; done
EOF
chmod +x "$ROOT/usr/bin/slowloop.sh"
cat > "$ROOT/etc/schema-init/services/test-slow.svc" <<'EOF'
name=test-slow
exec=/usr/bin/slowloop.sh
EOF
cat > "$ROOT/etc/schema-init/services/test-after.svc" <<'EOF'
name=test-after
exec=/usr/bin/touch
args=/run/test-after.ran
oneshot=1
dep=test-slow
EOF
# test-leaf: timer-promoted after test-after, nothing depends on it. It must
# not set the boot total; analyze lists it as not counted.
cat > "$ROOT/etc/schema-init/services/test-leaf.svc" <<'EOF'
name=test-leaf
exec=/usr/bin/slowloop.sh
stable_secs=2
dep=test-after
EOF

# Group gate: test-viagrp waits on group test-group (test-slow + test-readypath)
# and on the timer test-timer. The chain must walk into the group to test-slow,
# and the timer dep must not be reported as never ready.
cat > "$ROOT/etc/schema-init/services/test-group.grp" <<'EOF'
name=test-group
member=test-slow
member=test-readypath
EOF
cat > "$ROOT/etc/schema-init/services/test-viagrp.svc" <<'EOF'
name=test-viagrp
exec=/usr/bin/touch
args=/run/test-viagrp.ran
oneshot=1
dep=test-group
dep=test-timer
EOF

# env= spawn probe: prove the Phase 2 env= key reaches the child's environment.
cat > "$ROOT/usr/bin/envprobe.sh" <<'EOF'
#!/bin/sh
echo "ENVPROBE=[$MYVAR]" > /dev/console
echo "PATHPROBE=[$(busybox tr '\0' '\n' < /proc/$$/environ | grep '^PATH=')]" > /dev/console
EOF
chmod +x "$ROOT/usr/bin/envprobe.sh"
cat > "$ROOT/etc/schema-init/services/test-env.svc" <<'EOF'
name=test-env
exec=/usr/bin/envprobe.sh
env=MYVAR=itworks
oneshot=1
needs_root=1
EOF

# Drop-in probe: test-dropin.svc.d resets args=, overrides env=, sets mem_limit=.
cat > "$ROOT/usr/bin/dropprobe.sh" <<'EOF'
#!/bin/sh
echo "DROPIN-PROBE arg=[$1] argc=[$#] var=[$DVAR] mem=[$(cat /sys/fs/cgroup/schema-init/test-dropin/memory.max 2>&1)]" > /dev/console
EOF
chmod +x "$ROOT/usr/bin/dropprobe.sh"
cat > "$ROOT/etc/schema-init/services/test-dropin.svc" <<'EOF'
name=test-dropin
exec=/usr/bin/dropprobe.sh
args=base-arg
env=DVAR=base
oneshot=1
needs_root=1
EOF
mkdir -p "$ROOT/etc/schema-init/services/test-dropin.svc.d"
cat > "$ROOT/etc/schema-init/services/test-dropin.svc.d/10-override.conf" <<'EOF'
args=
args=dropin-arg
env=
env=DVAR=dropin
mem_limit=64
EOF
echo 'mem_limit=999' > "$ROOT/etc/schema-init/services/test-dropin.svc.d/.#20-lock.conf"

# Coredump probe: the VM boots with the kernel's default "core" pattern, as
# blakbox does. PID 1 should take it and raise RLIMIT_CORE before the first
# spawn, so a first-wave service already runs with an unlimited soft limit.
printf '#!/bin/sh\ncat > /dev/null\n' > "$ROOT/usr/bin/schema-coredump"
chmod +x "$ROOT/usr/bin/schema-coredump"
cat > "$ROOT/usr/bin/coreprobe.sh" <<'EOF'
#!/bin/sh
echo "COREPROBE pattern=[$(cat /proc/sys/kernel/core_pattern)] self=[$(grep 'core file' /proc/self/limits)] pid1=[$(grep 'core file' /proc/1/limits)]" > /dev/console
EOF
chmod +x "$ROOT/usr/bin/coreprobe.sh"
cat > "$ROOT/etc/schema-init/services/test-coreprobe.svc" <<'EOF'
name=test-coreprobe
exec=/usr/bin/coreprobe.sh
oneshot=1
needs_root=1
EOF

cat > "$ROOT/etc/schema-init/services/test-iso.svc" <<'EOF'
name=test-iso
exec=/bin/sleep
args=600
cpuset=3
cpuset_partition=isolated
EOF
cat > "$ROOT/etc/schema-init/services/test-root.svc" <<'EOF'
name=test-root
exec=/bin/sleep
args=600
cpuset=2
cpuset_partition=root
EOF
cat > "$ROOT/etc/schema-init/services/test-share.svc" <<'EOF'
name=test-share
exec=/bin/sleep
args=600
EOF
cat > "$ROOT/etc/schema-init/services/test-iso2.svc" <<'EOF'
name=test-iso2
exec=/bin/sleep
args=600
cpuset=3
cpuset_partition=isolated
dep=test-iso
EOF
cat > "$ROOT/etc/schema-init/services/test-noset.svc" <<'EOF'
name=test-noset
exec=/bin/sleep
args=600
cpuset_partition=isolated
EOF
# Phase 1 service hardening: opt-in no_new_privs + keep_caps. Runs as a plain
# root child (no run_uid), so this exercises the capset-on-a-root-service path.
# keep_caps=CAP_NET_BIND_SERVICE (value 10) => CapBnd must collapse to 0x400.
cat > "$ROOT/etc/schema-init/services/test-hardened.svc" <<'EOF'
name=test-hardened
exec=/bin/sleep
args=600
no_new_privs=1
keep_caps=CAP_NET_BIND_SERVICE
EOF

# args= left-trim regression guard: a leading space in the value must be
# stripped before it reaches argv, so a stranger writing `args= foo` gets
# "foo", not " foo". The script brackets $1 so a stray leading space is
# visible in the serial. Broken parse => "[ TRIMMED]"; fixed => "[TRIMMED]".
cat > "$ROOT/usr/bin/argtrim.sh" <<'EOF'
#!/bin/sh
echo "[$1]" > /run/argtrim.val
EOF
chmod +x "$ROOT/usr/bin/argtrim.sh"
cat > "$ROOT/etc/schema-init/services/test-argtrim.svc" <<'EOF'
name=test-argtrim
exec=/usr/bin/argtrim.sh
args= TRIMMED
oneshot=1
needs_root=1
EOF

# Phase 2 chrony hardening: the REAL self-privdrop path. test_privdrop replays
# chronyd's exact privileged sequence (bind :123 -> chown /run -> write pidfile
# as root -> setgid/setuid -> retain CAP_SYS_TIME) under the 6-cap keep set. The
# root pidfile write into the chrony-owned 0750 dir needs CAP_DAC_OVERRIDE -- the
# cap whose absence crash-looped the first hardened boot. A /bin/sleep here would
# false-green -- it exercises none of those -- which was exactly the Phase-1 gap.
cat > "$ROOT/etc/schema-init/services/test-privdrop.svc" <<'EOF'
name=test-privdrop
exec=/bin/test_privdrop
needs_root=1
no_new_privs=1
keep_caps=CAP_SYS_TIME,CAP_NET_BIND_SERVICE,CAP_CHOWN,CAP_SETUID,CAP_SETGID,CAP_DAC_OVERRIDE
EOF

# Phase 3 mount-ns: prep gives /boot/efi its OWN mount (so a non-recursive
# read-only would leave it writable) and puts content in /home and /root. The
# same checker runs hardened ("on") and plain ("off"); "off" must see every
# probe OPEN or the probes prove nothing. No /efi exists: exercises ENOENT skip.
cat > "$ROOT/usr/bin/mountns-prep.sh" <<'EOF'
#!/bin/sh
mount -t tmpfs efi /boot/efi
mount -t tmpfs home /home
mkdir /home/jon
touch /home/jon/secret /root/secret
EOF
chmod +x "$ROOT/usr/bin/mountns-prep.sh"
cat > "$ROOT/etc/schema-init/services/test-mountns-prep.svc" <<'EOF'
name=test-mountns-prep
exec=/usr/bin/mountns-prep.sh
oneshot=1
needs_root=1
EOF
cat > "$ROOT/etc/schema-init/services/test-mountns-on.svc" <<'EOF'
name=test-mountns-on
exec=/bin/test_mountns
args=on
dep=test-mountns-prep
needs_root=1
private_tmp=1
protect_system=full
protect_home=1
EOF
cat > "$ROOT/etc/schema-init/services/test-mountns-off.svc" <<'EOF'
name=test-mountns-off
exec=/bin/test_mountns
args=off
dep=test-mountns-prep
needs_root=1
EOF

cat > "$ROOT/etc/schema-init/services/docker-modules.svc" <<'EOF'
name=docker-modules
exec=/usr/local/bin/docker-modules.sh
oneshot=1
needs_root=1
critical=0
EOF

cat > "$ROOT/etc/schema-init/services/docker.svc" <<'EOF'
name=docker
exec=/usr/bin/dockerd
dep=docker-modules
needs_root=1
critical=0
ready_path=/var/run/docker.sock
EOF

mkdir -p "$ROOT/usr/local/bin"
cat > "$ROOT/usr/local/bin/docker-modules.sh" <<'EOF'
#!/bin/sh
echo "mock docker-modules running"
mkdir -p /sys/fs/cgroup/schema-init/docker-modules
cat /sys/fs/cgroup/schema-init/docker-modules/cgroup.controllers > /run/docker-modules.controllers
touch /run/docker-modules.ran
EOF
chmod +x "$ROOT/usr/local/bin/docker-modules.sh"

mkdir -p "$ROOT/usr/bin"
cat > "$ROOT/usr/bin/dockerd" <<'EOF'
#!/bin/sh
echo "mock dockerd running"
mkdir -p /var/run
sleep 1
touch /var/run/docker.sock
while true; do
    sleep 5
done
EOF
chmod +x "$ROOT/usr/bin/dockerd"

# Finisher: dep=test-dependent, so it runs the instant the dependent svc
# completes (i.e. just after the 90s start-timeout excise) — dumps the marker
# files to serial and powers off, ending the run early instead of idling to
# the timeout. Gating on the dep avoids both the late-fire and early-kill races.
cat > "$ROOT/usr/bin/vmfinish.sh" <<'EOF'
#!/bin/sh
exec >/dev/console 2>&1   # service stdout is captured to a logfile; force serial
echo "===== VMTEST-REPORT ====="
ls -la /run/test-timer.fired /run/test-dependent.ran 2>&1
[ -d /run/systemd/system ] && echo "SDBOOTED-DIR: present" || echo "SDBOOTED-DIR: MISSING"
# NB: this initramfs only has the applets linked above -- no wc, find or sed.
RAIL=/var/log/schema-init/rail.log
[ -f "$RAIL" ] || RAIL=/run/log/schema-init/rail.log
echo "RAIL-LOG: $RAIL"
grep -E 'timer-fire|start-timeout|oneshot-done' "$RAIL" 2>&1 | while read -r l; do
    echo "RAIL| $l"   # prefix so the assertion cannot match the console's own copy
done
echo "argtrim-val: $(cat /run/argtrim.val 2>&1)"
echo "===== NOFILE-TEST ====="
# PID 1 must raise its OWN soft limit to the hard one, and children must get
# the original soft limit back -- a raised soft NOFILE breaks select()/fd_set.
set -- $(grep 'Max open files' /proc/1/limits)
P_SOFT=$4; P_HARD=$5
echo "PID1-NOFILE: soft=$P_SOFT hard=$P_HARD"
# Resolve a supervised child HERE -- ISO_PID is not computed until the cpuset
# section below, and reading it early made the child assertion compare two
# empty strings and pass vacuously.
NOFILE_PID=$(head -1 /sys/fs/cgroup/schema-init/test-iso/cgroup.procs 2>/dev/null)
set -- $(grep 'Max open files' /proc/$NOFILE_PID/limits 2>/dev/null)
C_SOFT=$4; C_HARD=$5
echo "CHILD-NOFILE: pid=$NOFILE_PID soft=$C_SOFT hard=$C_HARD"
if [ -z "$C_SOFT" ]; then
    echo "NOFILE-CHILD: FAIL (could not read a child's limits)"
elif [ "$C_SOFT" != "$P_SOFT" ]; then
    echo "NOFILE-CHILD: PASS (child kept the original soft $C_SOFT)"
else
    echo "NOFILE-CHILD: FAIL (child inherited the raised soft $C_SOFT)"
fi
if [ "$P_SOFT" = "$P_HARD" ]; then
    echo "NOFILE-PID1: PASS (soft raised to hard)"
else
    echo "NOFILE-PID1: FAIL (soft $P_SOFT != hard $P_HARD)"
fi
echo "===== NOFILE-END ====="
echo "===== CTL-RESTART-TEST ====="
RP_BEFORE=$(cat /run/test-rp.pid 2>/dev/null)
/bin/schema-ctl restart test-readypath
RP_AFTER=$RP_BEFORE
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    sleep 1
    RP_AFTER=$(cat /run/test-rp.pid 2>/dev/null)
    [ -n "$RP_AFTER" ] && [ "$RP_AFTER" != "$RP_BEFORE" ] && [ -e /run/test-rp.ready ] && break
done
echo "rp-pid: $RP_BEFORE -> $RP_AFTER"
if grep -q 'test-readypath .*readiness-lost' "$RAIL"; then
    echo "CTL-RESTART: FAIL (readiness-lost on a requested restart)"
elif [ -n "$RP_BEFORE" ] && [ "$RP_AFTER" != "$RP_BEFORE" ]; then
    echo "CTL-RESTART: PASS (respawned, new pid)"
else
    echo "CTL-RESTART: FAIL (no respawn)"
fi
echo "===== CTL-RESTART-END ====="
echo "===== STALE-READY-TEST ====="
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    [ "$(cat /run/test-stale.n 2>/dev/null)" = 2 ] && [ -e /run/test-stale.ready ] && break
    sleep 1
done
sleep 2
echo "stale-runs: $(cat /run/test-stale.n 2>/dev/null)"
if grep -q 'test-stale .*readiness-lost' "$RAIL"; then
    echo "STALE-READY: FAIL (leftover marker promoted the respawn, then readiness-lost)"
elif [ "$(cat /run/test-stale.n 2>/dev/null)" = 2 ] && /bin/schema-ctl status test-stale | grep -q FUNDAMENTAL; then
    echo "STALE-READY: PASS (respawn promoted on its own marker)"
else
    echo "STALE-READY: FAIL ($(/bin/schema-ctl status test-stale | head -1))"
fi
if [ "$(cat /run/test-stale2.n 2>/dev/null)" = 2 ] && /bin/schema-ctl status test-stale2 | grep -q FUNDAMENTAL; then
    echo "STALE-INPLACE: PASS (in-place rewrite promoted the respawn)"
else
    echo "STALE-INPLACE: FAIL ($(cat /run/test-stale2.n 2>/dev/null) $(/bin/schema-ctl status test-stale2 | head -1))"
fi
echo "===== STALE-READY-END ====="
echo "===== RELOAD-REFIRE-TEST ====="
# SIGHUP to PID 1 is the reload path (init.c calls handle_reload from the
# signal drain), which is what schema-ctl reload asks for over the socket.
# Using the signal keeps this test free of schema-ctl, which is dynamically
# linked and has no libc in this initramfs.
BEFORE=$(grep -c fired /run/runonce.count 2>/dev/null || echo 0)
echo "RUNONCE-BEFORE: $BEFORE"
kill -HUP 1
sleep 3
AFTER=$(grep -c fired /run/runonce.count 2>/dev/null || echo 0)
echo "RUNONCE-AFTER: $AFTER"
if [ "$BEFORE" = "$AFTER" ]; then
    echo "RELOAD-REFIRE: PASS (run-once boot timer stayed terminal)"
else
    echo "RELOAD-REFIRE: FAIL (re-fired $BEFORE -> $AFTER)"
fi
echo "===== RELOAD-REFIRE-END ====="
echo "===== CPUSET-REPORT ====="
echo "root-subtree: $(cat /sys/fs/cgroup/cgroup.subtree_control 2>&1)"
echo "schema-init-subtree: $(cat /sys/fs/cgroup/schema-init/cgroup.subtree_control 2>&1)"
echo "docker-modules-controllers: $(cat /run/docker-modules.controllers 2>&1)"
echo "docker-sock: $(ls -la /var/run/docker.sock 2>&1)"
echo "iso-partition: $(cat /sys/fs/cgroup/schema-init/test-iso/cpuset.cpus.partition 2>&1)"
ISO_PID=$(head -1 /sys/fs/cgroup/schema-init/test-iso/cgroup.procs 2>/dev/null)
echo "iso-affinity: $(grep Cpus_allowed_list /proc/$ISO_PID/status 2>&1)"
# test-iso is /bin/sleep -- a plain execv'd binary that never touches its own
# mask, the same shape as crond. PID 1 runs with SIGCHLD+SIGHUP blocked and the
# mask survives exec, so this is where the inheritance shows up.
echo "sigmask-child: $(grep SigBlk /proc/$ISO_PID/status 2>&1)"
HARD_PID=$(head -1 /sys/fs/cgroup/schema-init/test-hardened/cgroup.procs 2>/dev/null)
echo "hardened-nnp: $(grep NoNewPrivs /proc/$HARD_PID/status 2>&1)"
echo "hardened-capbnd: $(grep CapBnd /proc/$HARD_PID/status 2>&1)"
PRIVDROP_PID=$(head -1 /sys/fs/cgroup/schema-init/test-privdrop/cgroup.procs 2>/dev/null)
echo "privdrop-result: $(cat /run/chrony-test/ok 2>&1)"
echo "privdrop-uid: $(grep -E '^Uid:' /proc/$PRIVDROP_PID/status 2>&1)"
echo "privdrop-nnp: $(grep NoNewPrivs /proc/$PRIVDROP_PID/status 2>&1)"
echo "privdrop-capbnd: $(grep CapBnd /proc/$PRIVDROP_PID/status 2>&1)"
echo "privdrop-capeff: $(grep CapEff /proc/$PRIVDROP_PID/status 2>&1)"
echo "root-partition: $(cat /sys/fs/cgroup/schema-init/test-root/cpuset.cpus.partition 2>&1)"
echo "share-effective: $(cat /sys/fs/cgroup/schema-init/test-share/cpuset.cpus.effective 2>&1)"
echo "iso2-partition: $(cat /sys/fs/cgroup/schema-init/test-iso2/cpuset.cpus.partition 2>&1)"
echo "schema-excl: $(cat /sys/fs/cgroup/schema-init/cpuset.cpus.exclusive 2>&1)"
echo "===== CPUSET-END ====="
echo "===== MOUNTNS-REPORT ====="
echo "mountns-on: $(cat /run/mountns-on 2>&1)"
echo "mountns-off: $(cat /run/mountns-off 2>&1)"
[ -e /tmp/mountns-sentinel-on ] && echo "MOUNTNS-TMP: FAIL (private /tmp leaked to host)" \
                                || echo "MOUNTNS-TMP: PASS"
[ -e /tmp/mountns-sentinel-off ] && echo "MOUNTNS-TMP-CONTROL: PASS" \
                                 || echo "MOUNTNS-TMP-CONTROL: FAIL"
ON_PID=$(head -1 /sys/fs/cgroup/schema-init/test-mountns-on/cgroup.procs 2>/dev/null)
NS_ON=$(readlink /proc/$ON_PID/ns/mnt 2>&1); NS_1=$(readlink /proc/1/ns/mnt 2>&1)
echo "mountns-ns: on=$NS_ON pid1=$NS_1"
[ -n "$ON_PID" ] && [ "$NS_ON" != "$NS_1" ] && echo "MOUNTNS-NS: PASS" || echo "MOUNTNS-NS: FAIL"
touch /usr/.host-probe && echo "MOUNTNS-HOSTUSR: PASS" || echo "MOUNTNS-HOSTUSR: FAIL"
mount -t tmpfs late /home && touch /home/late-file
touch /run/mountns-recheck
sleep 3
echo "mountns-late-on: $(cat /run/mountns-late-on 2>&1)"
echo "mountns-late-off: $(cat /run/mountns-late-off 2>&1)"
echo "===== MOUNTNS-END ====="
echo "ENVFILE-OK: $(cat /run/test-envfile.out 2>&1)"
chmod g+w /etc/test-env
rm -f /run/test-envfile.out
/bin/schema-ctl restart test-envfile >/dev/null 2>&1
i=0; while [ $i -lt 20 ] && ! grep -qs 'REFUSED env_file' /run/log/schema-init/test-envfile.log /var/log/schema-init/test-envfile.log; do sleep 0.5; i=$((i+1)); done
echo "ENVFILE-UNSAFE: $(cat /run/test-envfile.out 2>&1 | head -1) | $(grep -h 'REFUSED env_file' /run/log/schema-init/test-envfile.log /var/log/schema-init/test-envfile.log 2>/dev/null | head -1)"
rm -f /run/test-envopt.out
/bin/schema-ctl restart test-envopt >/dev/null 2>&1
i=0; while [ $i -lt 20 ] && [ ! -s /run/test-envopt.out ]; do sleep 0.5; i=$((i+1)); done
echo "ENVOPT-UNSAFE: $(cat /run/test-envopt.out 2>&1)"
chmod g-w /etc/test-env
chmod g+w /etc/schema-init/services/test-readypath.svc
echo "UNSAFE-RELOAD: $(/bin/schema-ctl reload 2>&1 | head -1)"
echo "UNSAFE-AFTER: $(/bin/schema-ctl status test-readypath 2>&1 | head -1)"
chmod g-w /etc/schema-init/services/test-readypath.svc
echo "mem_limit=32" > /etc/schema-init/services/test-dropin.svc.d/30-late.conf
echo "DROPIN-RELOAD: $(/bin/schema-ctl reload 2>&1 | head -1)"
MPA_PRE=$(/bin/schema-ctl status mp-after 2>&1 | head -1 | grep -o ' pid=[0-9]*' | head -1 | cut -d= -f2)
echo "DROPIN-REEXEC: $(/bin/schema-ctl reexec 2>&1 | head -1)"
sleep 2
echo "MP-reexec: $( [ -n "$MPA_PRE" ] && [ "$MPA_PRE" = "$(/bin/schema-ctl status mp-after 2>&1 | head -1 | grep -o ' pid=[0-9]*' | head -1 | cut -d= -f2)" ] && [ "$MPA_PRE" = "$(cat /run/mp-after.pid)" ] && echo kept || echo "lost ($MPA_PRE)")"
echo "DROPIN-CAT-BEGIN"; /bin/schema-ctl cat test-dropin; echo "DROPIN-CAT-END"
mpst() { /bin/schema-ctl status "$1" 2>&1 | head -1; }
mppid() { mpst "$1" | grep -o ' pid=[0-9]*' | head -1 | cut -d= -f2; }
echo "MP-before: $( [ "$(mppid mp-before)" = "$(cat /run/mp-before.pid)" ] && echo adopted || echo "no ($(mpst mp-before) want $(cat /run/mp-before.pid))")"
echo "MP-after: $( [ "$(mppid mp-after)" = "$(cat /run/mp-after.pid)" ] && echo adopted || echo "no ($(mpst mp-after) want $(cat /run/mp-after.pid))")"
echo "MP-fail: $(grep -Ec 'mp-fail .*(died|76-no-restart)' "$RAIL") adopted=$(grep -c 'mp-fail .*main-pid pid' "$RAIL") child=$(kill -0 "$(cat /run/mp-fail.pid)" 2>/dev/null && echo alive || echo gone)"
echo "MP-never: $(grep -c 'mp-never .*main-pid-timeout' "$RAIL")"
echo "MP-outside: $(grep -c 'mp-outside .*main-pid-timeout' "$RAIL") adopted=$(grep -c 'mp-outside .*main-pid pid' "$RAIL") rejected=$(grep -c 'mp-outside .*main-pid-rejected pid=1 invalid' "$RAIL")"
echo "MP-restart: $(grep -c 'mp-restart .*main-pid pid' "$RAIL")"
echo "MP-norestart: $(grep -c 'mp-norestart .*main-pid pid' "$RAIL") $(mpst mp-norestart | grep -o 'state=[A-Z_]*')"
echo "MP-nokey: $( [ "$(mppid mp-nokey)" = "$(cat /run/mp-nokey.pid)" ] && echo adopted || echo not-adopted)"
echo "MP-cleanexit: timeouts=$(grep -c 'mp-cleanexit .*main-pid-timeout' "$RAIL") $(mpst mp-cleanexit | grep -o 'state=[A-Z_]*')"
WT0=$(grep -c 'mp-waitstop .*main-pid-timeout' "$RAIL")
rm -f /run/mp-waitstop.post
/bin/schema-ctl reset mp-waitstop >/dev/null 2>&1
sleep 2
/bin/schema-ctl stop mp-waitstop >/dev/null 2>&1
sleep 12
echo "MP-waitstop: new-timeouts=$(( $(grep -c 'mp-waitstop .*main-pid-timeout' "$RAIL") - WT0 )) $(mpst mp-waitstop | grep -o 'state=[A-Z_]*') post=$(cat /run/mp-waitstop.post 2>/dev/null || echo none)"
MPR=$(mppid mp-after)
/bin/schema-ctl restart mp-after >/dev/null 2>&1
i=0; while [ $i -lt 20 ] && kill -0 "$MPR" 2>/dev/null; do sleep 0.25; i=$((i+1)); done
echo "MP-restart-old: $(kill -0 "$MPR" 2>/dev/null && echo alive || echo gone)"
MPB=$(cat /run/mp-before.pid)
/bin/schema-ctl stop mp-before >/dev/null 2>&1
i=0; while [ $i -lt 20 ] && kill -0 "$MPB" 2>/dev/null; do sleep 0.25; i=$((i+1)); done
echo "MP-stop: $(kill -0 "$MPB" 2>/dev/null && echo alive || echo gone)"
for d in /sys/fs/cgroup/schema-init/verify-ok /sys/fs/cgroup/schema-init/verify-ok/*/; do [ -d "$d" ] && echo "VERIFY-TREE: $d mem=$(cat $d/memory.max 2>&1) ev=$(grep populated $d/cgroup.events 2>&1)"; done
echo "VERIFY-OK: $(/bin/schema-ctl status verify-ok 2>&1 | head -1 | grep -o 'state=[A-Z_]*') mismatch=$(grep -c 'verify-ok .*limits-mismatch' "$RAIL")"
echo "VERIFY-NOREADY: $(/bin/schema-ctl status verify-noready 2>&1 | head -1 | grep -o 'state=[A-Z_]*')"
echo "VERIFY-BAD: $(/bin/schema-ctl status verify-bad 2>&1 | head -1 | grep -o 'state=[A-Z_]*') mismatch=$(grep -c 'verify-bad .*limits-mismatch' "$RAIL")"
echo "CGFORK: $(/bin/schema-ctl status test-cgfork 2>&1 | head -1 | grep -o 'state=[A-Z_]*') daemon=$(kill -0 "$(cat /run/cgfork.pid)" 2>/dev/null && echo alive || echo dead)"
CGN=$(cat /run/cgnest.path 2>/dev/null)
echo "CGNEST-UP: $( [ -n "$CGN" ] && [ -d "$CGN/runtime" ] && echo nested || echo "no ($CGN)")"
/bin/schema-ctl restart test-cgnest >/dev/null 2>&1
sleep 3
echo "CGNEST-RESTART: $(/bin/schema-ctl status test-cgnest 2>&1 | head -1 | grep -o 'state=[A-Z_]*') runtime=$( [ -d "$CGN/runtime" ] && echo present || echo gone) procs=$(cat "$CGN/runtime/cgroup.procs" 2>/dev/null | wc -l)"
/bin/schema-ctl stop test-cgnest >/dev/null 2>&1
sleep 2
echo "CGNEST-STOP: $( [ -d "$CGN" ] && echo "left $(ls -d "$CGN"/*/ 2>/dev/null | wc -l) children" || echo removed)"
/bin/schema-ctl stop test-stopcmd >/dev/null 2>&1
i=0; while [ $i -lt 20 ] && ! grep -qs post /run/stopprobe.order; do sleep 0.5; i=$((i+1)); done
echo "STOPCMD-ORDER: $(tr '\n' ' ' < /run/stopprobe.order 2>&1)"
echo "===== RESTARTS-TEST ====="
echo "crash-retries: $(grep -c 'test-crash .*retry-deep' "$RAIL")"
echo "crash-dormant: $(grep -c 'test-crash .*dormant ' "$RAIL")"
echo "crashdep-spawn: $(grep -c 'test-crashdep .*spawn' "$RAIL")"
echo "hangdep-promote: $(grep -c 'test-hangdep .*promote' "$RAIL")"
echo "rplost-lost: $(grep -c 'test-rplost .*readiness-lost' "$RAIL")"
echo "rplost-dormant: $(grep -c 'test-rplost .*dormant' "$RAIL")"
echo "rplost-spawn: $(grep -c 'test-rplost .*spawn' "$RAIL")"
echo "rplost-state: $(/bin/schema-ctl status test-rplost | head -1)"
for s in test-crash test-timer test-readypath test-dependent; do
    echo "RESTARTS $s $(/bin/schema-ctl status $s | head -1)"
done
echo "===== ANALYZE-TEST ====="
/bin/schema-ctl analyze
echo "ANALYZE-ONE-BEGIN"
/bin/schema-ctl analyze test-after
echo "ANALYZE-ONE-END"
echo "ANALYZE-GRP-BEGIN"
/bin/schema-ctl analyze test-viagrp
echo "ANALYZE-GRP-END"
echo "ANALYZE-MISSING: $(/bin/schema-ctl analyze no-such-svc 2>&1 | head -1)"
echo "===== VMTEST-END ====="
# Exercise schema-init's OWN shutdown rail (SIGINT = reboot), not the kernel's.
# poweroff -f would bypass PID 1 and leave the shutdown path untested.
# Fallback in the background so a wedged shutdown still ends the run.
( sleep 45; echo "SHUTDOWN-WEDGED: forcing poweroff"; poweroff -f ) &
kill -INT 1
EOF
chmod +x "$ROOT/usr/bin/vmfinish.sh"
cat > "$ROOT/etc/schema-init/services/test-finish.svc" <<'EOF'
name=test-finish
exec=/usr/bin/vmfinish.sh
oneshot=1
needs_root=1
dep=test-dependent
dep=docker
EOF

# 3. Pack initramfs.
( cd "$ROOT" && find . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -9 ) > "$WORK/initramfs.cpio.gz"
echo ">> initramfs: $(du -h "$WORK/initramfs.cpio.gz" | cut -f1)"

# 4. Boot it.
echo ">> booting QEMU (timeout ${TIMEOUT}s, kernel $(uname -r))..."
timeout "$TIMEOUT" qemu-system-x86_64 \
  -enable-kvm -m 512 -smp 4 -no-reboot -nographic -nic none \
  -kernel "$KERNEL" -initrd "$WORK/initramfs.cpio.gz" \
  -append "console=ttyS0 rdinit=/sbin/schema-init panic=1 loglevel=4" \
  < /dev/null >"$SERIAL" 2>&1 || true

# 5. Verdict.  Always keep the serial so there's an artifact to inspect.
cp "$SERIAL" "$OUT/last-vmtest-serial.log"
echo; echo "================ SERIAL TAIL ================"
sed -n '/VMTEST-REPORT/,/VMTEST-END/p' "$SERIAL" || true
echo "============================================="
# Source of truth is the boot rail itself, not the (best-effort) reporter dump.
pass=1
grep -Eq "test-timer .*timer-fire"      "$SERIAL" || { echo "  MISS: timer-fire"; pass=0; }
grep -Eq "test-timer .*timer-done"      "$SERIAL" || { echo "  MISS: timer-done"; pass=0; }
grep -Eq "test-hang .*start-timeout"    "$SERIAL" || { echo "  MISS: start-timeout"; pass=0; }
grep -Eq "test-dependent .*(spawn|oneshot-done)" "$SERIAL" || { echo "  MISS: dependent ran"; pass=0; }
grep -Eq "SDBOOTED-DIR: present"        "$SERIAL" || { echo "  MISS: /run/systemd/system (sd_booted signal)"; pass=0; }
grep -Fq "PATHPROBE=[PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin]" "$SERIAL" || { echo "  MISS: PID1 did not hand services a default PATH"; pass=0; }
# The rail must outlive the console it was printed on.
grep -Eq "RAIL\| .*test-hang .*start-timeout" "$SERIAL" || { echo "  MISS: rail.log did not persist the rail"; pass=0; }
# A completed run-once boot timer must stay terminal across a reload.
grep -Eq "RUNONCE-BEFORE: 1"   "$SERIAL" || { echo "  MISS: run-once boot timer never fired"; pass=0; }
grep -Eq "CTL-RESTART: PASS"  "$SERIAL" || { echo "  MISS: schema-ctl restart of a ready_path svc did not respawn it"; pass=0; }
grep -Eq "RELOAD-REFIRE: PASS" "$SERIAL" || { echo "  MISS: reload re-fired a completed run-once timer"; pass=0; }
# PID 1 raises its own NOFILE; children must not inherit the raised soft limit.
# args= leading space must be trimmed off before argv.
grep -Eq "argtrim-val: \[TRIMMED\]" "$SERIAL" || { echo "  MISS: args= leading space not trimmed (got non-[TRIMMED])"; pass=0; }
grep -Eq "NOFILE-PID1: PASS"  "$SERIAL" || { echo "  MISS: PID 1 did not raise its own RLIMIT_NOFILE"; pass=0; }
grep -Eq "NOFILE-CHILD: PASS" "$SERIAL" || { echo "  MISS: child inherited PID 1's raised NOFILE soft limit"; pass=0; }
grep -Eq "iso-partition: isolated"                  "$SERIAL" || { echo "  MISS: iso partition not isolated"; pass=0; }
grep -Eq "iso-affinity:.*Cpus_allowed_list:[[:space:]]*3" "$SERIAL" || { echo "  MISS: iso affinity != core 3"; pass=0; }
grep -Eq "root-partition: root"                     "$SERIAL" || { echo "  MISS: root variant did not form partition"; pass=0; }
grep -Eq "share-effective: 0-1"                     "$SERIAL" || { echo "  MISS: sibling still sees an exclusive core"; pass=0; }
grep -Eq "iso2-partition: member"                   "$SERIAL" || { echo "  MISS: overlapping iso2 did not degrade"; pass=0; }
grep -Eq "HAZARD: 'test-iso2' cpuset_partition=isolated rejected" "$SERIAL" || { echo "  MISS: degrade HAZARD not logged"; pass=0; }
grep -Eq "WARN: 'test-noset' cpuset_partition set without cpuset" "$SERIAL" || { echo "  MISS: empty-cpuset normalization warn"; pass=0; }
subtree_has() {
  # $1=label $2=controller — kernel prints canonical order (cpuset cpu io memory pids),
  # so match each controller independently rather than assuming an order.
  grep -E "^$1:" "$SERIAL" | grep -Eq "(^|[[:space:]])$2([[:space:]]|\$)"
}
for ctrl in pids io; do
  subtree_has "root-subtree" "$ctrl"              || { echo "  MISS: root cgroup subtree_control missing $ctrl"; pass=0; }
  subtree_has "schema-init-subtree" "$ctrl"       || { echo "  MISS: schema-init cgroup subtree_control missing $ctrl"; pass=0; }
  subtree_has "docker-modules-controllers" "$ctrl" || { echo "  MISS: docker-modules cgroup missing $ctrl delegation"; pass=0; }
done
grep -Eq "docker-sock:.*docker.sock"                "$SERIAL" || { echo "  MISS: docker.sock not found"; pass=0; }
# no $ anchor: QEMU's serial line ends CRLF and the CR is part of the line.
grep -Eq "sigmask-child:.*SigBlk:[[:space:]]*0{16}" "$SERIAL" || { echo "  MISS: child inherited PID 1's blocked signal mask (SIGCHLD) across exec"; pass=0; }
# Phase 1 hardening: no_new_privs applied, and keep_caps=CAP_NET_BIND_SERVICE
# collapsed the bounding set to exactly 0x400 (bit 10). Proves parse -> capbset
# drop -> capset all ran correctly under real PID 1 on a root-staying child.
grep -Eq "hardened-nnp:.*NoNewPrivs:[[:space:]]*1"          "$SERIAL" || { echo "  MISS: hardened service NoNewPrivs != 1"; pass=0; }
grep -Eq "hardened-capbnd:.*CapBnd:[[:space:]]*0000000000000400" "$SERIAL" || { echo "  MISS: hardened CapBnd != CAP_NET_BIND_SERVICE only"; pass=0; }
# Phase 2 chrony: the self-privdrop actually SUCCEEDS under the 6-cap keep set.
# Anti-false-green: test_privdrop writes /run/chrony-test/ok and reaches uid 996
# only if every cap-gated step (bind/chown/pidfile-as-root/setgid/setuid/adjtimex)
# is permitted -- the pidfile write is the CAP_DAC_OVERRIDE regression guard.
grep -Eq "privdrop-result: PRIVDROP_OK"                 "$SERIAL" || { echo "  MISS: privdrop helper did not complete (cap set too tight?)"; pass=0; }
grep -Eq "privdrop-uid:.*Uid:[[:space:]]*996"           "$SERIAL" || { echo "  MISS: privdrop helper did not drop to uid 996"; pass=0; }
grep -Eq "privdrop-nnp:.*NoNewPrivs:[[:space:]]*1"      "$SERIAL" || { echo "  MISS: privdrop NoNewPrivs != 1"; pass=0; }
grep -Eq "privdrop-capbnd:.*CapBnd:[[:space:]]*00000000020004c3" "$SERIAL" || { echo "  MISS: privdrop CapBnd != 6-cap set"; pass=0; }
grep -Eq "privdrop-capeff:.*CapEff:[[:space:]]*0000000002000000" "$SERIAL" || { echo "  MISS: privdrop CapEff != CAP_SYS_TIME after drop"; pass=0; }
# Phase 3 mount-ns. "off" is the red half: every probe must be OPEN there.
grep -Eq "mountns-on: usr=RO etc=RO efi=RO home=EMPTY root=EMPTY tmpwrite=OK" "$SERIAL" || { echo "  MISS: hardened service view not fully isolated"; pass=0; }
grep -Eq "mountns-off: usr=OPEN etc=OPEN efi=OPEN home=OPEN root=OPEN tmpwrite=OK" "$SERIAL" || { echo "  MISS: control run not fully open (probes are vacuous)"; pass=0; }
grep -Eq "MOUNTNS-TMP: PASS"          "$SERIAL" || { echo "  MISS: private /tmp leaked to host"; pass=0; }
grep -Eq "MOUNTNS-TMP-CONTROL: PASS"  "$SERIAL" || { echo "  MISS: control /tmp write not visible on host"; pass=0; }
grep -Eq "MOUNTNS-NS: PASS"           "$SERIAL" || { echo "  MISS: hardened service shares PID 1's mount ns"; pass=0; }
grep -Eq "MOUNTNS-HOSTUSR: PASS"      "$SERIAL" || { echo "  MISS: host /usr went read-only"; pass=0; }
grep -Eq "mountns-late-on: home=EMPTY" "$SERIAL" || { echo "  MISS: late host mount on /home leaked into protect_home view"; pass=0; }
grep -Eq "mountns-late-off: home=OPEN" "$SERIAL" || { echo "  MISS: late-mount control did not see the new /home"; pass=0; }
# Shutdown rail: every step must print, and PID 1 must reach reboot() itself.
# A wedge here is the 2026-07-26 hang (unbounded sync never returned).
for step in "services down in" "cgroups killed" "control socket and shm released" \
            "sync done" "filesystems read-only"; do
  grep -Eq "shutdown: $step" "$SERIAL" || { echo "  MISS: shutdown step '$step'"; pass=0; }
done
# .svc.d drop-ins: applied at boot, and a drop-in added after boot is "modified".
grep -Eq "DROPIN-PROBE arg=\[dropin-arg\] argc=\[1\] var=\[dropin\] mem=\[67108864\]" "$SERIAL" || { echo "  MISS: drop-in not applied (args reset / env reset / mem_limit)"; pass=0; }
grep -Eq "ENVOPT-UNSAFE: ENVO=\[\]" "$SERIAL" || { echo "  MISS: optional group-writable env_file not skipped"; pass=0; }
grep -Eq "ENVFILE-OK: ENVF=from-file" "$SERIAL" || { echo "  MISS: env_file not loaded"; pass=0; }
grep -Eq "ENVFILE-UNSAFE: .*No such file.*REFUSED env_file /etc/test-env" "$SERIAL" || { echo "  MISS: group-writable env_file not refused"; pass=0; }
grep -Eq "UNSAFE-RELOAD: err: .* 1 .svc file\(s\) or service dir\(s\) refused" "$SERIAL" || { echo "  MISS: reload not refused with a group-writable .svc"; pass=0; }
grep -Eq "UNSAFE-AFTER: .*FUNDAMENTAL" "$SERIAL" || { echo "  MISS: refused reload evicted a running service"; pass=0; }
grep -Eq "DROPIN-RELOAD: err: .*'test-dropin' modified since boot" "$SERIAL" || { echo "  MISS: reload not refused after a drop-in was added"; pass=0; }
grep -Eq "DROPIN-REEXEC: err: .*test-dropin" "$SERIAL" || { echo "  MISS: reexec not refused after a drop-in was added"; pass=0; }
grep -Eq "^# /etc/schema-init/services/test-dropin.svc.d/30-late.conf" "$SERIAL" || { echo "  MISS: schema-ctl cat did not list the late drop-in"; pass=0; }
# restart_count: first spawn, timer fires and ctl restart are not restarts.
for c in "MP-before: adopted" "MP-after: adopted" "MP-fail: [1-9][0-9]* adopted=0 child=gone" "MP-never: [1-9]" \
         "MP-outside: [1-9][0-9]* adopted=0 rejected=[1-9]" "MP-restart: [2-9]" "MP-norestart: 1 state=EXCISED" \
         "MP-nokey: not-adopted" "MP-stop: gone" "MP-reexec: kept" \
         "MP-cleanexit: timeouts=0 state=EXCISED" "MP-waitstop: new-timeouts=0 state=EXCISED post=ran" "MP-restart-old: gone"; do
    grep -Eq "$c" "$SERIAL" || { echo "  MISS: main_pid_notify: $c ($(grep -o "${c%%:*}:.*" "$SERIAL" | head -1))"; pass=0; }
done
grep -Eq "VERIFY-OK: state=FUNDAMENTAL mismatch=0" "$SERIAL" || { echo "  MISS: verify-ok ($(grep -o 'VERIFY-OK:.*' "$SERIAL"))"; pass=0; }
grep -Eq "VERIFY-NOREADY: state=FULL_TRUST" "$SERIAL" || { echo "  MISS: verify-noready promoted without READY=1, limits unchecked ($(grep -o 'VERIFY-NOREADY:.*' "$SERIAL"))"; pass=0; }
grep -Eq "VERIFYCG \+memory failed" "$SERIAL" && { echo "  MISS: verify fixture could not enable memory"; pass=0; }
grep -Eq "VERIFY-BAD: state=EXCISED mismatch=1" "$SERIAL" || { echo "  MISS: verify-bad ran without its limit ($(grep -o 'VERIFY-BAD:.*' "$SERIAL"))"; pass=0; }
grep -Eq "CGFORK: state=FUNDAMENTAL daemon=alive" "$SERIAL" || { echo "  MISS: nested pid_file daemon killed at handover ($(grep -o 'CGFORK:.*' "$SERIAL"))"; pass=0; }
grep -Eq "CGNEST-UP: nested" "$SERIAL" || { echo "  MISS: test-cgnest never nested its cgroup"; pass=0; }
grep -Eq "CGNEST-RESTART: state=[A-Z_]* runtime=present procs=[1-9]" "$SERIAL" || { echo "  MISS: nested service lost its cgroup on restart ($(grep -o 'CGNEST-RESTART:.*' "$SERIAL"))"; pass=0; }
grep -Eq "CGNEST-STOP: removed" "$SERIAL" || { echo "  MISS: stop left the nested cgroup tree ($(grep -o 'CGNEST-STOP:.*' "$SERIAL"))"; pass=0; }
grep -Eq "STOPCMD-ORDER: stop term post" "$SERIAL" || { echo "  MISS: exec_stop / SIGTERM / exec_stop_post out of order"; pass=0; }
grep -Eq "crash-retries: 3"  "$SERIAL" || { echo "  MISS: test-crash did not retry exactly max_restarts=3 times"; pass=0; }
grep -Eq "crash-dormant: [1-9]" "$SERIAL" || { echo "  MISS: test-crash never went dormant"; pass=0; }
grep -Eq "crashdep-spawn: [1-9]" "$SERIAL" || { echo "  MISS: dependent of a dormant non-critical dep never started"; pass=0; }
grep -Eq "hangdep-promote: [1-9]" "$SERIAL" || { echo "  MISS: dependent of an excised non-critical dep never promoted"; pass=0; }
grep -Eq "rplost-lost: [1-9]"   "$SERIAL" || { echo "  MISS: test-rplost readiness-lost not detected"; pass=0; }
grep -Eq "rplost-dormant: 0"    "$SERIAL" || { echo "  MISS: test-rplost went dormant after readiness-lost"; pass=0; }
grep -Eq "rplost-spawn: [2-9]"  "$SERIAL" || { echo "  MISS: test-rplost not respawned after readiness-lost"; pass=0; }
grep -Eq "rplost-state: .*FUNDAMENTAL" "$SERIAL" || { echo "  MISS: test-rplost not back to FUNDAMENTAL"; pass=0; }
grep -Eq "RESTARTS test-crash .*restarts=3"     "$SERIAL" || { echo "  MISS: test-crash restarts != 3"; pass=0; }
grep -Eq "RESTARTS test-timer .*restarts=0"     "$SERIAL" || { echo "  MISS: timer firings counted as restarts"; pass=0; }
grep -Eq "RESTARTS test-readypath .*restarts=0" "$SERIAL" || { echo "  MISS: schema-ctl restart counted as a restart"; pass=0; }
grep -Eq "RESTARTS test-dependent .*restarts=0" "$SERIAL" || { echo "  MISS: first spawn counted as a restart"; pass=0; }
# analyze: summary, chain walk through a dep, readiness sources, timer hint.
grep -Eq "^boot: kernel [0-9.]+s \+ userspace [0-9.]+s = "   "$SERIAL" || { echo "  MISS: analyze boot summary"; pass=0; }
grep -Eq "^critical chain → "                                "$SERIAL" || { echo "  MISS: analyze critical chain"; pass=0; }
grep -Eq "test-readypath +[0-9.]+s +[0-9.]+s  ready_path"    "$SERIAL" || { echo "  MISS: analyze readiness source ready_path"; pass=0; }
grep -Eq "test-dependent +[0-9.]+s +[0-9.]+s  exit"          "$SERIAL" || { echo "  MISS: analyze readiness source exit"; pass=0; }
grep -Eq "test-slow +[0-9.]+s +(9|10)\.[0-9]+s  timer"  "$SERIAL" || { echo "  MISS: analyze timer promotion (stable_secs=10, whole-second check) for test-slow"; pass=0; }
sed -n '/ANALYZE-ONE-BEGIN/,/ANALYZE-ONE-END/p' "$SERIAL" | grep -Eq "^ +└─test-slow @" || { echo "  MISS: analyze test-after chain did not walk to test-slow"; pass=0; }
grep -Eq "^not counted in boot .*test-leaf"                     "$SERIAL" || { echo "  MISS: analyze did not exclude timer leaf test-leaf"; pass=0; }
grep -Eq "^timer-gated with dependents .*test-slow"          "$SERIAL" || { echo "  MISS: analyze did not flag test-slow as timer-gated"; pass=0; }
grep -Eq "^ +└─test-hang never ready \(EXCISED\)"            "$SERIAL" || { echo "  MISS: analyze chain did not name the never-ready dep"; pass=0; }
sed -n '/ANALYZE-GRP-BEGIN/,/ANALYZE-GRP-END/p' "$SERIAL" | grep -Eq "^ +└─test-slow \(via test-group\) @" || { echo "  MISS: analyze chain did not walk into group test-group"; pass=0; }
sed -n '/ANALYZE-GRP-BEGIN/,/ANALYZE-GRP-END/p' "$SERIAL" | grep -q "test-timer" && { echo "  MISS: analyze reported a timer dep as never ready"; pass=0; }
grep -Eq "ANALYZE-MISSING: err: not found: no-such-svc"      "$SERIAL" || { echo "  MISS: analyze unknown service not rejected"; pass=0; }
grep -Eq "PID 1 reboot"     "$SERIAL" || { echo "  MISS: PID 1 never reached reboot()"; pass=0; }
grep -Eq "SHUTDOWN-WEDGED"  "$SERIAL" && { echo "  MISS: shutdown wedged, forced off"; pass=0; }
if [ "$pass" = 1 ]; then
  echo ">> RESULT: PASS  (timer fired, hang excised at timeout, dependent ran anyway)"
else
  echo ">> RESULT: FAIL / INCONCLUSIVE — full serial saved:"
  cp "$SERIAL" "$OUT/last-vmtest-serial.log"
  echo "   $OUT/last-vmtest-serial.log"
  trap - EXIT; echo "   workdir kept: $WORK"
fi
