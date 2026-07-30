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

# 2. Assemble a minimal initramfs root.
mkdir -p "$ROOT"/{sbin,bin,usr/bin,etc/schema-init/services,proc,sys,dev,run,sys/fs/cgroup}
cp "$BIN" "$ROOT/sbin/schema-init"
ln -sf /sbin/schema-init "$ROOT/init"   # kernel initramfs entry point is /init
cp "$BB"  "$ROOT/bin/busybox"
for a in sh ls cat sleep touch poweroff mount mkdir echo grep head kill; do
  ln -sf /bin/busybox "$ROOT/bin/$a"
done
ln -sf /bin/busybox "$ROOT/usr/bin/touch"   # test svcs call /usr/bin/touch
ln -sf /bin/busybox "$ROOT/bin/sleep"       # and /bin/sleep

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
echo "root-partition: $(cat /sys/fs/cgroup/schema-init/test-root/cpuset.cpus.partition 2>&1)"
echo "share-effective: $(cat /sys/fs/cgroup/schema-init/test-share/cpuset.cpus.effective 2>&1)"
echo "iso2-partition: $(cat /sys/fs/cgroup/schema-init/test-iso2/cpuset.cpus.partition 2>&1)"
echo "schema-excl: $(cat /sys/fs/cgroup/schema-init/cpuset.cpus.exclusive 2>&1)"
echo "===== CPUSET-END ====="
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
( cd "$ROOT" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$WORK/initramfs.cpio.gz"
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
# The rail must outlive the console it was printed on.
grep -Eq "RAIL\| .*test-hang .*start-timeout" "$SERIAL" || { echo "  MISS: rail.log did not persist the rail"; pass=0; }
# A completed run-once boot timer must stay terminal across a reload.
grep -Eq "RUNONCE-BEFORE: 1"   "$SERIAL" || { echo "  MISS: run-once boot timer never fired"; pass=0; }
grep -Eq "RELOAD-REFIRE: PASS" "$SERIAL" || { echo "  MISS: reload re-fired a completed run-once timer"; pass=0; }
# PID 1 raises its own NOFILE; children must not inherit the raised soft limit.
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
# Shutdown rail: every step must print, and PID 1 must reach reboot() itself.
# A wedge here is the 2026-07-26 hang (unbounded sync never returned).
for step in "SIGTERM sent" "cgroups killed" "control socket and shm released" \
            "sync done" "filesystems read-only"; do
  grep -Eq "shutdown: $step" "$SERIAL" || { echo "  MISS: shutdown step '$step'"; pass=0; }
done
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
