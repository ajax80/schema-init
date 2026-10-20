#!/bin/bash
# notifycap-vm.sh <schema-init> <schema-ctl> <old|new> <serial-out>
# An unprivileged sender fills PID 1's fd closers with SO_LINGER sockets, then
# floods the notify socket with fds. old = pre-6.16 simulated by a seccomp
# wrapper failing setsockopt(SO_PASSRIGHTS); softdog stands in for hardware.
# Pass: no watchdog-abort, no start-timeout, no softdog reboot, <= 33 closers.
set -e
BIN="$1"; CTL="$2"; MODE="$3"; OUT="$4"; H="$(cd "$(dirname "$0")" && pwd)/notifycap"
B0="$(mktemp -d)"; trap 'rm -rf "$B0"' EXIT
for x in notifier fdattack; do cc -static -O2 -D_GNU_SOURCE -o "$B0/$x" "$H/$x.c"; done
cc -static -O2 -D_GNU_SOURCE $([ "$MODE" = old ] && echo -DOLD) -o "$B0/preinit" "$H/preinit.c"
zstd -dqf /lib/modules/$(uname -r)/kernel/drivers/watchdog/softdog.ko.zst -o "$B0/softdog.ko"
KERNEL=/lib/modules/$(uname -r)/vmlinuz; [ -r "$KERNEL" ] || KERNEL=/boot/vmlinuz-$(uname -r)
WORK=$(mktemp -d /var/tmp/schema-ncapvm.XXXXXX); ROOT=$WORK/root
trap 'rm -rf "$WORK" "$B0"' EXIT
mkdir -p "$ROOT"/{sbin,bin,lib,usr/bin,etc/schema-init/services,proc,sys,dev,run,tmp,sys/fs/cgroup}
cp "$BIN" "$ROOT/sbin/schema-init"; cp "$CTL" "$ROOT/usr/bin/schema-ctl"
cp "$B0/preinit" "$ROOT/init"; cp "$B0/softdog.ko" "$ROOT/lib/"
cp "$B0/notifier" "$B0/fdattack" "$ROOT/usr/bin/"
cp /usr/sbin/busybox "$ROOT/bin/busybox"
for a in sh cat sleep echo cut kill grep wc touch; do ln -sf /bin/busybox "$ROOT/bin/$a"; done
S=$ROOT/etc/schema-init/services; B=$ROOT/usr/bin
printf '#!/bin/sh\nexec /usr/bin/notifier READY=1 pets\n' > $B/wdog.sh
printf '#!/bin/sh\nsleep 8\necho "LATE READY sent $(cut -d" " -f1 /proc/uptime)" >/dev/console\nexec /usr/bin/notifier READY=1 hold\n' > $B/late.sh
cat > $B/mon.sh <<'M'
#!/bin/sh
max=0
for t in $(seq 1 50); do
  n=$(grep -l '^schema-init' /proc/[0-9]*/comm 2>/dev/null | wc -l); n=$((n-1))
  [ $n -gt $max ] && max=$n
  [ $((t % 5)) = 0 ] && echo "MON t=$(cut -d' ' -f1 /proc/uptime) closers=$n" >/dev/console
  sleep 1
done
echo "MAXCLOSERS $max" >/dev/console
/usr/bin/schema-ctl status 2>&1 | grep -E 'wdog|late' | sed 's/^/ST /' >/dev/console
echo "POWEROFF-AT $(cut -d' ' -f1 /proc/uptime)" >/dev/console
kill -USR2 1
while :; do sleep 1; done
M
printf 'name=wdog\nexec=/usr/bin/wdog.sh\nnotify=1\nwatchdog_sec=3\nwatchdog_timeout_ms=4000\n' > $S/wdog.svc
printf 'name=late\nexec=/usr/bin/late.sh\nnotify=1\nstart_timeout_sec=10\nstable_secs=600\n' > $S/late.svc
printf 'name=attack\nexec=/usr/bin/fdattack\nneeds_root=1\n' > $S/attack.svc
printf 'name=mon\nexec=/usr/bin/mon.sh\nneeds_root=1\n' > $S/mon.svc
ln -sf /bin/busybox "$ROOT/bin/seq"
chmod +x $B/*.sh; chmod -R go-w "$ROOT"
( cd "$ROOT" && find . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -9 ) > "$WORK/i.gz"
timeout 120 qemu-system-x86_64 -enable-kvm -m 512 -smp 2 -no-reboot -nographic -nic none \
  -kernel "$KERNEL" -initrd "$WORK/i.gz" \
  -append "console=ttyS0 rdinit=/init panic=1 loglevel=4" </dev/null >"$OUT" 2>&1 || true
grep -aE "PREINIT|ATTACK|LATE|MON|MAXCLOSERS|^ST |POWEROFF|notify: reads|watchdog|start-timeout|notify-ready|promote|excised|softdog|reboot|PID 1 poweroff|Kernel panic" "$OUT" | grep -v 'opened hardware' | head -60
