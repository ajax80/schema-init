#!/bin/bash
# reload --evict grace force-kill under QEMU: evict-vm.sh <schema-init> <schema-ctl> <serial-out>
set -e
BIN="$1"; CTL="$2"; OUT="$3"
KERNEL=/lib/modules/$(uname -r)/vmlinuz; [ -r "$KERNEL" ] || KERNEL=/boot/vmlinuz-$(uname -r)
WORK=$(mktemp -d /var/tmp/schema-evictvm.XXXXXX); ROOT=$WORK/root
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$ROOT"/{sbin,bin,usr/bin,etc/schema-init/services,proc,sys,dev,run,tmp,sys/fs/cgroup}
cp "$BIN" "$ROOT/sbin/schema-init"; cp "$CTL" "$ROOT/usr/bin/schema-ctl"; ln -sf /sbin/schema-init "$ROOT/init"
cp /usr/sbin/busybox "$ROOT/bin/busybox"
for a in sh cat sleep echo cut kill grep awk sed wc touch rm; do ln -sf /bin/busybox "$ROOT/bin/$a"; done
S=$ROOT/etc/schema-init/services; B=$ROOT/usr/bin
cat > $B/stub.sh <<'EOF'
#!/bin/sh
trap 'echo "STUB-TERM-IGNORED $(cut -d" " -f1 /proc/uptime)" >/dev/console' TERM
echo $$ > /run/stub.pid
sh -c 'trap "" TERM; echo $$ > /run/stubkid.pid; exec sleep 1000' &
while :; do sleep 1; done
EOF
printf '#!/bin/sh\ntrap "echo POLITE-TERM \\$(cut -d\\" \\" -f1 /proc/uptime) >/dev/console; exit 0" TERM\necho $$ > /run/polite.pid\nwhile :; do sleep 1 & wait $!; done\n' > $B/polite.sh
printf 'name=stub\nexec=/usr/bin/stub.sh\n' > $S/stub.svc
printf 'name=polite\nexec=/usr/bin/polite.sh\n' > $S/polite.svc
cat > $B/watch.sh <<'EOF'
#!/bin/sh
up() { cut -d' ' -f1 /proc/uptime; }
alive() { [ -d /proc/$1 ] && ! grep -q '^State:.*Z' /proc/$1/status && echo alive || echo gone; }
sleep 5
SP=$(cat /run/stub.pid); KP=$(cat /run/stubkid.pid); PP=$(cat /run/polite.pid)
echo "PIDS stub=$SP kid=$KP polite=$PP" >/dev/console
echo "CG stub=$(cat /proc/$SP/cgroup) kid=$(cat /proc/$KP/cgroup)" >/dev/console
rm /etc/schema-init/services/stub.svc /etc/schema-init/services/polite.svc
echo "RELOAD-AT $(up)" >/dev/console
/usr/bin/schema-ctl reload --evict >/dev/console 2>&1
sleep 1
echo "T+1 $(up) stub=$(alive $SP) kid=$(alive $KP) polite=$(alive $PP)" >/dev/console
sleep 1
echo "T+2 $(up) stub=$(alive $SP) kid=$(alive $KP) polite=$(alive $PP)" >/dev/console
sleep 4
echo "T+6 $(up) stub=$(alive $SP) kid=$(alive $KP) polite=$(alive $PP)" >/dev/console
/usr/bin/schema-ctl status 2>&1 | grep -E 'stub|polite' | sed 's/^/ST /' >/dev/console
echo "POWEROFF-AT $(up)" >/dev/console
kill -USR2 1
while :; do sleep 1; done
EOF
printf 'name=watcher\nexec=/usr/bin/watch.sh\nneeds_root=1\n' > $S/watcher.svc
chmod +x $B/*.sh
( cd "$ROOT" && find . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -9 ) > "$WORK/i.gz"
timeout 90 qemu-system-x86_64 -enable-kvm -m 512 -smp 2 -no-reboot -nographic -nic none \
  -kernel "$KERNEL" -initrd "$WORK/i.gz" \
  -append "console=ttyS0 rdinit=/init panic=1 loglevel=4" </dev/null >"$OUT" 2>&1 || true
grep -aE "^(PIDS|CG|RELOAD|T\+|ST |POWEROFF|STUB|POLITE|ok:|error)|evict|orphaned|unmanaged|force-killed|PID 1 poweroff|Kernel panic" "$OUT"
