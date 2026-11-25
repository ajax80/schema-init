#!/bin/sh
# CI stages 1-2: the real schema-init boots as PID 1 in a minimal QEMU guest, starts
# services in after=/before= order, recovers a crashing one (its dependent waits for the recovery), re-execs
# itself with the running services kept, and powers off through its own shutdown path. Busybox initramfs, no
# disk; the host's kernel (KERNEL=). Exit 0 only on a clean boot and poweroff.
set -eu
# the guest refuses group/world-writable .svc files; a umask of 002 (Fedora private groups) would make them so
umask 022
cd "$(dirname "$0")/../.."
REPO=$PWD
KERNEL=${KERNEL:-/boot/vmlinuz-$(uname -r)}
BB=${BUSYBOX:-$(command -v busybox)}
TIMEOUT=${TIMEOUT:-150}
WORK=$(mktemp -d)
ROOT=$WORK/root
SERIAL=$WORK/serial.log
trap 'rm -rf "$WORK"' EXIT

[ -r "$KERNEL" ] || { echo "FAIL: kernel $KERNEL not readable"; exit 1; }
# the guest has no libc; file says "static-pie linked" for static-pie
static() { file -L "$1" | grep -Eq 'static(ally|-pie) linked'; }
static "$BB" || { echo "FAIL: $BB is not a static busybox"; exit 1; }

make -s schema-init
# make goes by timestamps: a sanitizer build (STATIC_LDFLAGS=) would look up to date
static schema-init || { echo "FAIL: ./schema-init is not static; make clean first"; exit 1; }
# schema-ctl is dynamic in the tree; the guest has no libc
SCTL_SRCS=$(make -s --no-print-directory -f Makefile -f - print-sctl-srcs <<'MK'
print-sctl-srcs:
	@echo $(SCHEMA_CTL_SRCS)
MK
)
# shellcheck disable=SC2086
cc -static -O2 -std=c99 -D_GNU_SOURCE -I. -DSCHEMA_INIT_VERSION='"ci"' -o "$WORK/schema-ctl" $SCTL_SRCS -lrt

mkdir -p "$ROOT"/bin "$ROOT"/sbin "$ROOT"/usr/bin "$ROOT"/etc/schema-init/services \
         "$ROOT"/dev "$ROOT"/proc "$ROOT"/sys "$ROOT"/run "$ROOT"/tmp "$ROOT"/var/log
cp schema-init "$ROOT/sbin/schema-init"
cp "$WORK/schema-ctl" "$ROOT/bin/schema-ctl"
cp "$BB" "$ROOT/bin/busybox"
for a in sh cat echo sleep poweroff touch grep head cut stat; do ln -s busybox "$ROOT/bin/$a"; done
# security regressions: a schema-group user for the control-socket gate, and an
# initrd-style handoff with umask 0 (#204: systemd's initrd hands PID 1 umask 0)
printf 'root:x:0:0::/:/bin/sh\nciuser:x:1000:1000::/tmp:/bin/sh\n' > "$ROOT/etc/passwd"
printf 'root:x:0:\nschema:x:900:ciuser\nciuser:x:1000:\n' > "$ROOT/etc/group"
chmod 1777 "$ROOT/tmp"
printf '#!/bin/sh\numask 000\nexec /sbin/schema-init\n' > "$ROOT/sbin/init-umask0"
chmod +x "$ROOT/sbin/init-umask0"

svc() { printf '%s\n' "$2" > "$ROOT/etc/schema-init/services/$1.svc"; }
svc ci-hello 'name=ci-hello
exec=/bin/sh
args=-c
args=echo CI-HELLO > /dev/console
oneshot=1'
# a running service the shutdown must stop with SIGTERM, not leave to the final kill
printf '#!/bin/sh\ntrap "echo CI-DAEMON-TERM > /dev/console; exit 0" TERM\ntouch /run/ci-daemon.ready\nwhile :; do sleep 1; done\n' > "$ROOT/bin/ci-daemon"
chmod +x "$ROOT/bin/ci-daemon"
# ready once its trap is set, so ci-finish's poweroff can't beat it under TCG
svc ci-daemon 'name=ci-daemon
exec=/bin/ci-daemon
ready_path=/run/ci-daemon.ready'
# crashes twice, then stays up: two counted restarts, then ready
printf '#!/bin/sh\nn=$(cat /run/ci-crash.n 2>/dev/null || echo 0); n=$((n+1)); echo $n > /run/ci-crash.n\n[ $n -lt 3 ] && { echo CI-CRASH-$n > /dev/console; exit 1; }\ntouch /run/ci-crash.ready\nwhile :; do sleep 1; done\n' > "$ROOT/bin/ci-crasher"
chmod +x "$ROOT/bin/ci-crasher"
svc ci-crasher 'name=ci-crasher
exec=/bin/ci-crasher
max_restarts=5
ready_path=/run/ci-crash.ready'
# must start only once ci-crasher has recovered, never on one of its dead runs
printf '#!/bin/sh\nn=$(cat /run/ci-crash.n 2>/dev/null)\n[ "$n" = 3 ] && echo CI-DEP-AFTER-RECOVERY > /dev/console || echo "CI-DEP-EARLY n=$n" > /dev/console\n' > "$ROOT/bin/ci-dependent"
chmod +x "$ROOT/bin/ci-dependent"
svc ci-dependent 'name=ci-dependent
exec=/bin/ci-dependent
oneshot=1
dep=ci-crasher'
# after=/before= (ordering only): ci-ordered waits for ci-slow to finish starting and
# for ci-early, which names it in before=; ci-after-fail starts once ci-broken's
# first run has failed, and a name with no .svc holds nothing
printf '#!/bin/sh\nsleep 2\ntouch /run/ci-slow.ready\nwhile :; do sleep 1; done\n' > "$ROOT/bin/ci-slow"
chmod +x "$ROOT/bin/ci-slow"
svc ci-slow 'name=ci-slow
exec=/bin/ci-slow
ready_path=/run/ci-slow.ready'
svc ci-early 'name=ci-early
exec=/bin/sh
args=-c
args=sleep 1; touch /run/ci-early.done
oneshot=1
before=ci-ordered'
svc ci-ordered 'name=ci-ordered
exec=/bin/sh
args=-c
args=if [ -e /run/ci-slow.ready ] && [ -e /run/ci-early.done ]; then echo CI-AFTER-ORDERED; else echo CI-AFTER-EARLY; fi > /dev/console
oneshot=1
after=ci-slow
after=ci-missing'
svc ci-broken 'name=ci-broken
exec=/bin/sh
args=-c
args=echo CI-BROKEN-RAN > /dev/console; exit 1
max_restarts=1'
svc ci-after-fail 'name=ci-after-fail
exec=/bin/sh
args=-c
args=echo CI-AFTER-FAIL-RAN > /dev/console
oneshot=1
after=ci-broken'
# #204: a file a service creates must not be world-writable after a umask-0 handoff
svc ci-umask 'name=ci-umask
exec=/bin/sh
args=-c
args=touch /run/ci-umask.f; echo "CI-UMASK-$(stat -c %a /run/ci-umask.f)" > /dev/console
oneshot=1'
# the control socket: group schema may read (status), never change (stop); a stop
# that got through would also show as a second CI-DAEMON-TERM
printf '%s\n' '#!/bin/sh' 'o=/tmp/ci-gate.out' \
    'r=$(/bin/schema-ctl status ci-daemon 2>&1)' \
    'case "$r" in *pid=*) echo CI-GATE-READ-OK ;; *) echo "CI-GATE-READ-FAIL $r" ;; esac > $o' \
    'r=$(/bin/schema-ctl stop ci-daemon 2>&1)' \
    'case "$r" in *"requires root"*) echo CI-GATE-DENIED ;; *) echo "CI-GATE-BREACH $r" ;; esac >> $o' \
    > "$ROOT/bin/ci-gate"
chmod +x "$ROOT/bin/ci-gate"
svc ci-gate 'name=ci-gate
exec=/bin/ci-gate
user=ciuser
oneshot=1
dep=ci-daemon'
# schema-ctl poweroff, so the control socket and PID 1's own shutdown run; poweroff -f
# would bypass both. The fallback only ends a wedged run, and is a failure
cat > "$ROOT/bin/ci-finish" <<'EOF'
#!/bin/sh
echo CI-UP > /dev/console
echo "CI-SOCK-$(stat -c '%a-%G' /run/schema-init.sock)" > /dev/console
cat /tmp/ci-gate.out > /dev/console 2>&1
( sleep 90; echo CI-WEDGED > /dev/console; poweroff -f ) &
pid() { /bin/schema-ctl status "$1" 2>&1 | head -1 | grep -o 'pid=[0-9]*' | cut -d= -f2; }
D1=$(pid ci-daemon) C1=$(pid ci-crasher)
echo "CI-CRASHER: $(/bin/schema-ctl status ci-crasher 2>&1 | head -1)" > /dev/console
# --force: verify needs a bootloader this guest doesn't have; the re-exec itself is the test
/bin/schema-ctl reexec --force > /dev/console 2>&1
sleep 2
D2=$(pid ci-daemon) C2=$(pid ci-crasher)
if [ -n "$D1" ] && [ "$D1" = "$D2" ] && [ -d /proc/$D1 ] && [ -n "$C1" ] && [ "$C1" = "$C2" ]; then
    echo CI-REEXEC-KEPT > /dev/console
else
    echo "CI-REEXEC-LOST daemon $D1->$D2 crasher $C1->$C2" > /dev/console
fi
/bin/schema-ctl status > /dev/console 2>&1
/bin/schema-ctl poweroff > /dev/console 2>&1
EOF
chmod +x "$ROOT/bin/ci-finish"
svc ci-finish 'name=ci-finish
exec=/bin/ci-finish
oneshot=1
dep=ci-hello
dep=ci-daemon
dep=ci-dependent
dep=ci-ordered
dep=ci-after-fail
dep=ci-umask
dep=ci-gate'

( cd "$ROOT" && find . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -1 ) > "$WORK/initramfs.gz"

# the build host's CPU features, as the binary was built for it: qemu64 lacks AVX,
# which the runner's static glibc uses (PID 1 died of SIGILL)
ACCEL=tcg CPU=max
[ -w /dev/kvm ] && ACCEL=kvm CPU=host
# emulated, a boot can outlast ci-finish's 90 s wedge watchdog
[ "$ACCEL" = tcg ] && TIMEOUT=${TIMEOUT_TCG:-400}
echo ">> booting ($ACCEL, $(basename "$KERNEL"))"
rc=0
timeout "$TIMEOUT" qemu-system-x86_64 -accel "$ACCEL" -cpu "$CPU" -m 512 -smp 2 -no-reboot -nographic -nic none \
    -kernel "$KERNEL" -initrd "$WORK/initramfs.gz" \
    -append "console=ttyS0 rdinit=/sbin/init-umask0 panic=-1 loglevel=4" \
    < /dev/null > "$SERIAL" 2>&1 || rc=$?

pass=1
miss() { echo "  MISS: $1"; pass=0; }
[ "$rc" = 0 ] || miss "qemu exited $rc ($([ "$rc" = 124 ] && echo timeout || echo error))"
grep -q CI-HELLO "$SERIAL" || miss "oneshot ci-hello never ran"
grep -q CI-UMASK-644 "$SERIAL" || miss "#204: a service file after a umask-0 handoff is not 0644 ($(grep -o 'CI-UMASK-[0-9]*' "$SERIAL"))"
grep -q CI-SOCK-660-schema "$SERIAL" || miss "control socket is not 0660 root:schema ($(grep -o 'CI-SOCK-[^ ]*' "$SERIAL"))"
grep -q CI-GATE-READ-OK "$SERIAL" || miss "group schema could not run a read-only verb"
grep -q CI-GATE-DENIED "$SERIAL" || miss "group schema was not refused a mutating verb"
grep -q CI-GATE-BREACH "$SERIAL" && miss "group schema ran a mutating verb"
grep -q CI-UP "$SERIAL" || miss "ci-finish never ran after its deps were ready"
grep -q CI-CRASH-2 "$SERIAL" || miss "ci-crasher did not crash twice"
grep -Eq 'CI-CRASHER: .*restarts=2[^0-9]*$' "$SERIAL" || miss "ci-crasher's restarts are not 2"
grep -q CI-DEP-AFTER-RECOVERY "$SERIAL" || miss "ci-dependent did not wait for ci-crasher's recovery"
grep -q CI-DEP-EARLY "$SERIAL" && miss "ci-dependent started on a dead ci-crasher run"
grep -q CI-AFTER-ORDERED "$SERIAL" || miss "after=/before= did not hold ci-ordered until ci-slow was ready and ci-early done"
grep -q CI-AFTER-EARLY "$SERIAL" && miss "ci-ordered started before what it is ordered after"
grep -q CI-BROKEN-RAN "$SERIAL" || miss "ci-broken never ran"
grep -q CI-AFTER-FAIL-RAN "$SERIAL" || miss "after= on a failing service held its follower"
grep -Eq 're-exec .* [1-9][0-9]* adopted, 0 new' "$SERIAL" || miss "PID 1 did not re-exec and adopt its services"
# a rolled-back re-exec logs the same adopt line; only the client's reply differs
grep -q 'ok: re-executed into' "$SERIAL" || miss "re-exec not confirmed (rolled back?)"
grep -q CI-REEXEC-KEPT "$SERIAL" || miss "services did not survive the re-exec with the same pids"
[ "$(grep -c CI-DAEMON-TERM "$SERIAL")" = 1 ] || miss "ci-daemon got SIGTERM other than once at shutdown"
grep -q 'PID 1 poweroff' "$SERIAL" || miss "PID 1 never reached poweroff"
grep -q CI-WEDGED "$SERIAL" && miss "shutdown wedged, forced off"
grep -q 'Kernel panic' "$SERIAL" && miss "kernel panic (PID 1 died?)"
if [ "$pass" = 1 ]; then
    echo ">> PASS: PID 1 booted (umask-0 handoff), gated group schema to read-only, ordered after=/before=, recovered a crash in dependency order, re-exec kept services, powered off cleanly"
else
    echo ">> FAIL — serial:"
    cat "$SERIAL"
    exit 1
fi
