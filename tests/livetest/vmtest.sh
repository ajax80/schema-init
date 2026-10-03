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

# 2. Assemble a minimal initramfs root.
mkdir -p "$ROOT"/{sbin,bin,usr/bin,etc/schema-init/services,proc,sys,dev,run,sys/fs/cgroup}
cp "$BIN" "$ROOT/sbin/schema-init"
ln -sf /sbin/schema-init "$ROOT/init"   # kernel initramfs entry point is /init
cp "$BB"  "$ROOT/bin/busybox"
for a in sh ls cat sleep touch poweroff mount mkdir echo grep head kill readlink; do
  ln -sf /bin/busybox "$ROOT/bin/$a"
done
ln -sf /bin/busybox "$ROOT/usr/bin/touch"   # test svcs call /usr/bin/touch
ln -sf /bin/busybox "$ROOT/bin/sleep"       # and /bin/sleep
cp "$PRIVDROP" "$ROOT/bin/test_privdrop"
cp "$MOUNTNS" "$ROOT/bin/test_mountns"
cp "$SCTL" "$ROOT/bin/schema-ctl"
ln -sf /bin/busybox "$ROOT/bin/rm"

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
cat > "$ROOT/etc/schema-init/services/test-crash.svc" <<'EOF'
name=test-crash
exec=/usr/bin/crashloop.sh
max_restarts=3
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
echo "mem_limit=32" > /etc/schema-init/services/test-dropin.svc.d/30-late.conf
echo "DROPIN-RELOAD: $(/bin/schema-ctl reload 2>&1 | head -1)"
echo "DROPIN-REEXEC: $(/bin/schema-ctl reexec 2>&1 | head -1)"
echo "DROPIN-CAT-BEGIN"; /bin/schema-ctl cat test-dropin; echo "DROPIN-CAT-END"
echo "===== RESTARTS-TEST ====="
echo "crash-retries: $(grep -c 'test-crash .*retry-deep' "$RAIL")"
echo "crash-dormant: $(grep -c 'test-crash .*dormant ' "$RAIL")"
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
for step in "SIGTERM sent" "cgroups killed" "control socket and shm released" \
            "sync done" "filesystems read-only"; do
  grep -Eq "shutdown: $step" "$SERIAL" || { echo "  MISS: shutdown step '$step'"; pass=0; }
done
# .svc.d drop-ins: applied at boot, and a drop-in added after boot is "modified".
grep -Eq "DROPIN-PROBE arg=\[dropin-arg\] argc=\[1\] var=\[dropin\] mem=\[67108864\]" "$SERIAL" || { echo "  MISS: drop-in not applied (args reset / env reset / mem_limit)"; pass=0; }
grep -Eq "DROPIN-RELOAD: err: .*'test-dropin' modified since boot" "$SERIAL" || { echo "  MISS: reload not refused after a drop-in was added"; pass=0; }
grep -Eq "DROPIN-REEXEC: err: .*test-dropin" "$SERIAL" || { echo "  MISS: reexec not refused after a drop-in was added"; pass=0; }
grep -Eq "^# /etc/schema-init/services/test-dropin.svc.d/30-late.conf" "$SERIAL" || { echo "  MISS: schema-ctl cat did not list the late drop-in"; pass=0; }
# restart_count: first spawn, timer fires and ctl restart are not restarts.
grep -Eq "crash-retries: 3"  "$SERIAL" || { echo "  MISS: test-crash did not retry exactly max_restarts=3 times"; pass=0; }
grep -Eq "crash-dormant: [1-9]" "$SERIAL" || { echo "  MISS: test-crash never went dormant"; pass=0; }
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
