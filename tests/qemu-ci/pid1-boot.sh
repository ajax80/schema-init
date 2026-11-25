#!/bin/sh
# CI stage 1: the real schema-init boots as PID 1 in a minimal QEMU guest, starts
# services, and powers off through its own shutdown path. Busybox initramfs, no
# disk; the host's kernel (KERNEL=). Exit 0 only on a clean boot and poweroff.
set -eu
cd "$(dirname "$0")/../.."
REPO=$PWD
KERNEL=${KERNEL:-/boot/vmlinuz-$(uname -r)}
BB=${BUSYBOX:-$(command -v busybox)}
TIMEOUT=${TIMEOUT:-120}
WORK=$(mktemp -d)
ROOT=$WORK/root
SERIAL=$WORK/serial.log
trap 'rm -rf "$WORK"' EXIT

[ -r "$KERNEL" ] || { echo "FAIL: kernel $KERNEL not readable"; exit 1; }
file -L "$BB" | grep -q 'statically linked' || { echo "FAIL: $BB is not a static busybox"; exit 1; }

make -s schema-init
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
for a in sh cat echo sleep poweroff; do ln -s busybox "$ROOT/bin/$a"; done

svc() { printf '%s\n' "$2" > "$ROOT/etc/schema-init/services/$1.svc"; }
svc ci-hello 'name=ci-hello
exec=/bin/sh
args=-c
args=echo CI-HELLO > /dev/console
oneshot=1'
# a running service the shutdown must stop with SIGTERM, not leave to the final kill
printf '#!/bin/sh\ntrap "echo CI-DAEMON-TERM > /dev/console; exit 0" TERM\nwhile :; do sleep 1; done\n' > "$ROOT/bin/ci-daemon"
chmod +x "$ROOT/bin/ci-daemon"
svc ci-daemon 'name=ci-daemon
exec=/bin/ci-daemon'
# schema-ctl poweroff, so the control socket and PID 1's own shutdown run; poweroff -f
# would bypass both. The fallback only ends a wedged run, and is a failure
printf '#!/bin/sh\necho CI-UP > /dev/console\n/bin/schema-ctl status > /dev/console 2>&1\n( sleep 60; echo CI-WEDGED > /dev/console; poweroff -f ) &\n/bin/schema-ctl poweroff > /dev/console 2>&1\n' > "$ROOT/bin/ci-finish"
chmod +x "$ROOT/bin/ci-finish"
svc ci-finish 'name=ci-finish
exec=/bin/ci-finish
oneshot=1
dep=ci-hello
dep=ci-daemon'

( cd "$ROOT" && find . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -1 ) > "$WORK/initramfs.gz"

# the build host's CPU features, as the binary was built for it: qemu64 lacks AVX,
# which the runner's static glibc uses (PID 1 died of SIGILL)
ACCEL=tcg CPU=max
[ -w /dev/kvm ] && ACCEL=kvm CPU=host
echo ">> booting ($ACCEL, $(basename "$KERNEL"))"
rc=0
timeout "$TIMEOUT" qemu-system-x86_64 -accel "$ACCEL" -cpu "$CPU" -m 512 -smp 2 -no-reboot -nographic -nic none \
    -kernel "$KERNEL" -initrd "$WORK/initramfs.gz" \
    -append "console=ttyS0 rdinit=/sbin/schema-init panic=-1 loglevel=4" \
    < /dev/null > "$SERIAL" 2>&1 || rc=$?

pass=1
miss() { echo "  MISS: $1"; pass=0; }
[ "$rc" = 0 ] || miss "qemu exited $rc ($([ "$rc" = 124 ] && echo timeout || echo error))"
grep -q CI-HELLO "$SERIAL" || miss "oneshot ci-hello never ran"
grep -q CI-UP "$SERIAL" || miss "ci-finish never ran after its deps were ready"
grep -q CI-DAEMON-TERM "$SERIAL" || miss "shutdown did not SIGTERM ci-daemon"
grep -q 'PID 1 poweroff' "$SERIAL" || miss "PID 1 never reached poweroff"
grep -q CI-WEDGED "$SERIAL" && miss "shutdown wedged, forced off"
grep -q 'Kernel panic' "$SERIAL" && miss "kernel panic (PID 1 died?)"
if [ "$pass" = 1 ]; then
    echo ">> PASS: schema-init booted as PID 1, ran services, powered off cleanly"
else
    echo ">> FAIL — serial:"
    cat "$SERIAL"
    exit 1
fi
