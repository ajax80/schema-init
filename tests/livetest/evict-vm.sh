#!/bin/bash
# reload --evict grace force-kill under QEMU: [REEXEC=1] [NEXT=<old schema-init>] evict-vm.sh <schema-init> <schema-ctl> <serial-out>
# REEXEC/NEXT re-exec PID 1 (into itself / into NEXT) inside the grace window; READD_REEXEC=1 brings
# phoenix back with a re-exec instead of a reload; schema-ctl must be static
set -e
BIN="$1"; CTL="$2"; OUT="$3"
KERNEL=/lib/modules/$(uname -r)/vmlinuz; [ -r "$KERNEL" ] || KERNEL=/boot/vmlinuz-$(uname -r)
WORK=$(mktemp -d /var/tmp/schema-evictvm.XXXXXX); ROOT=$WORK/root
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$ROOT"/{sbin,bin,usr/bin,etc/schema-init/services,proc,sys,dev,run,tmp,sys/fs/cgroup}
cp "$BIN" "$ROOT/sbin/schema-init"; cp "$CTL" "$ROOT/usr/bin/schema-ctl"; ln -sf /sbin/schema-init "$ROOT/init"
cp /usr/sbin/busybox "$ROOT/bin/busybox"
for a in sh cat sleep echo cut kill grep awk sed wc touch rm cp; do ln -sf /bin/busybox "$ROOT/bin/$a"; done
S=$ROOT/etc/schema-init/services; B=$ROOT/usr/bin
cat > $B/stub.sh <<'EOF'
#!/bin/sh
trap 'echo "STUB-TERM-IGNORED $(cut -d" " -f1 /proc/uptime)" >/dev/console' TERM
echo $$ > /run/stub.pid
sh -c 'trap "" TERM; echo $$ > /run/stubkid.pid; exec sleep 1000' &
while :; do sleep 1; done
EOF
printf '#!/bin/sh\ntrap "echo POLITE-TERM \\$(cut -d\\" \\" -f1 /proc/uptime) >/dev/console; exit 0" TERM\necho $$ > /run/polite.pid\nwhile :; do sleep 1 & wait $!; done\n' > $B/polite.sh
cat > $B/leaky.sh <<'EOF'
#!/bin/sh
echo $$ > /run/leaky.pid
sh -c 'trap "" TERM; echo $$ > /run/leakykid.pid; exec sleep 1000' &
trap 'echo "LEAKY-TERM $(cut -d" " -f1 /proc/uptime)" >/dev/console; exit 0' TERM
while :; do sleep 1 & wait $!; done
EOF
printf 'name=stub\nexec=/usr/bin/stub.sh\n' > $S/stub.svc
printf 'name=leaky\nexec=/usr/bin/leaky.sh\n' > $S/leaky.svc
[ -n "${NEXT:-}" ] && { cp "$NEXT" "$ROOT/sbin/schema-init.next"; echo /sbin/schema-init.next > "$ROOT/etc/evict-reexec"; }
[ -n "${REEXEC:-}" ] && touch "$ROOT/etc/evict-reexec"
[ -n "${READD_REEXEC:-}" ] && touch "$ROOT/etc/readd-reexec"
printf 'name=polite\nexec=/usr/bin/polite.sh\n' > $S/polite.svc
cat > $B/phoenix.sh <<'EOF'
#!/bin/sh
echo $$ > /run/phoenix.pid
sh -c 'trap "" TERM; echo $$ > /run/phoenixkid.pid; exec sleep 1000' &
trap 'exit 0' TERM
while :; do sleep 1 & wait $!; done
EOF
printf 'name=phoenix\nexec=/usr/bin/phoenix.sh\n' > $S/phoenix.svc
cp $S/phoenix.svc $ROOT/etc/phoenix.svc.save
cat > $B/watch.sh <<'EOF'
#!/bin/sh
up() { cut -d' ' -f1 /proc/uptime; }
alive() { [ -d /proc/$1 ] && ! grep -q '^State:.*Z' /proc/$1/status && echo alive || echo gone; }
sleep 5
SP=$(cat /run/stub.pid); KP=$(cat /run/stubkid.pid); PP=$(cat /run/polite.pid); LP=$(cat /run/leaky.pid); LK=$(cat /run/leakykid.pid); XP=$(cat /run/phoenix.pid); XK=$(cat /run/phoenixkid.pid)
echo "PIDS stub=$SP kid=$KP polite=$PP leaky=$LP leakykid=$LK phoenix=$XP phoenixkid=$XK" >/dev/console
echo "CG stub=$(cat /proc/$SP/cgroup) kid=$(cat /proc/$KP/cgroup)" >/dev/console
rm /etc/schema-init/services/stub.svc /etc/schema-init/services/polite.svc /etc/schema-init/services/leaky.svc /etc/schema-init/services/phoenix.svc
echo "RELOAD-AT $(up)" >/dev/console
/usr/bin/schema-ctl reload --evict >/dev/console 2>&1
[ -f /etc/evict-reexec ] && { echo "REEXEC-AT $(up)" >/dev/console; /usr/bin/schema-ctl reexec $(cat /etc/evict-reexec) 2>&1 | head -3 >/dev/console; }
sleep 1
cp /etc/phoenix.svc.save /etc/schema-init/services/phoenix.svc
echo "READD-AT $(up)" >/dev/console
if [ -f /etc/readd-reexec ]; then /usr/bin/schema-ctl reexec 2>&1 | head -3 >/dev/console; else /usr/bin/schema-ctl reload >/dev/console 2>&1; fi
echo "T+1 $(up) stub=$(alive $SP) kid=$(alive $KP) polite=$(alive $PP) leaky=$(alive $LP) leakykid=$(alive $LK)" >/dev/console
sleep 1
echo "T+2 $(up) stub=$(alive $SP) kid=$(alive $KP) polite=$(alive $PP) leaky=$(alive $LP) leakykid=$(alive $LK) phoenixkid=$(alive $XK)" >/dev/console
sleep 4
echo "T+6 $(up) stub=$(alive $SP) kid=$(alive $KP) polite=$(alive $PP) leaky=$(alive $LP) leakykid=$(alive $LK) phoenixkid=$(alive $XK)" >/dev/console
NX=$(cat /run/phoenix.pid)
echo "PHOENIX old=$XP new=$NX new-$( [ "$NX" != "$XP" ] && echo $(alive $NX) || echo missing)" >/dev/console
d() { [ -d /sys/fs/cgroup/schema-init/$1 ] && echo present || echo absent; }
echo "CGDIRS stub=$(d stub) polite=$(d polite) leaky=$(d leaky) phoenix=$(d phoenix)" >/dev/console
/usr/bin/schema-ctl status 2>&1 | grep -E 'stub|polite|leaky|phoenix' | sed 's/^/ST /' >/dev/console
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
grep -aE "^(PIDS|CG|CGDIRS|PHOENIX|RELOAD|READD|REEXEC|T\+|ST |POWEROFF|STUB|POLITE|LEAKY|ok:|error)|back in the configuration|after SIGKILL|re-exec|evict|orphaned|unmanaged|force-killed|PID 1 poweroff|Kernel panic" "$OUT"
SP=$(grep -aoE '^PIDS stub=[0-9]+' "$OUT" | cut -d= -f2); PP=$(grep -aoE 'polite=[0-9]+' "$OUT" | head -1 | cut -d= -f2); LP=$(grep -aoE ' leaky=[0-9]+' "$OUT" | head -1 | cut -d= -f2)
fail=0
[ -n "$SP" ] && [ -n "$PP" ] && [ -n "$LP" ] || { echo "MISS: PIDS line"; fail=1; SP=x PP=x LP=x; }
grep -aqE "force-killed.*pid=$SP\b" "$OUT" || { echo "MISS: stub not force-killed"; fail=1; }
[ -n "${NEXT:-}" ] || { grep -aqE "force-killed.*pid=$PP\b" "$OUT" && { echo "BAD: polite exited on SIGTERM but was reported force-killed"; fail=1; }; }
[ -n "${NEXT:-}" ] || { grep -aqE "force-killed.*pid=$LP\b" "$OUT" && { echo "BAD: leaky main exited on SIGTERM but was reported force-killed"; fail=1; }; }
grep -aqE 'force-killed (leftovers in .*/leaky|pid='"$LP"'\b)' "$OUT" || { echo "MISS: leaky leftover child not force-killed"; fail=1; }
T6="stub=gone kid=gone polite=gone leaky=gone leakykid=gone"
grep -aqE "^T\+6 .*$T6" "$OUT" || { echo "MISS: not all gone at T+6"; fail=1; }
if [ -z "${NEXT:-}" ]; then
  grep -aq 'phoenix. is back in the configuration' "$OUT" || { echo "MISS: re-added phoenix leftovers not killed when it came back"; fail=1; }
  grep -aqE '^T\+2 .*phoenixkid=gone' "$OUT" || { echo "MISS: old phoenix kid alive after re-add"; fail=1; }
  grep -aqE '^PHOENIX .*new-alive' "$OUT" && grep -aqE '^ST +phoenix .*restarts=0' "$OUT" || { echo "BAD: re-added phoenix did not survive the old deadline"; fail=1; }
  grep -aqE '^CGDIRS stub=absent polite=absent leaky=absent phoenix=present' "$OUT" || { echo "MISS: evicted cgroup dirs not removed"; fail=1; }
fi
grep -aq 'PID 1 poweroff' "$OUT" || { echo "MISS: clean poweroff"; fail=1; }
[ $fail = 0 ] && echo ">> RESULT: PASS" || { echo ">> RESULT: FAIL"; exit 1; }
