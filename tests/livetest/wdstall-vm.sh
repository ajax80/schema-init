#!/bin/bash
# Hardware watchdog during a control-socket stall (the #479 audit's untested
# item). PID 1 boots in QEMU with a real i6300esb; a stuck reader and a
# dripping sender hold the socket for 75 s (2.5x the 30 s device timeout).
# PASS needs both halves:
#   STALL-OK      — the pets kept flowing: no reset while the clients stalled
#   DMT reset     — pets stop (a watchdog_timeout_ms service goes quiet) and
#                   the device then fires, proving it was armed all along.
# With -no-reboot the reset ends QEMU, so marker order in the serial decides.
set -e
REPO="${REPO:-$HOME/projects/schema-init}"
OUT="${OUT:-$HOME/schema-livetest/last-wdstall-serial.log}"
KERNEL=/lib/modules/$(uname -r)/vmlinuz; [ -r "$KERNEL" ] || KERNEL=/boot/vmlinuz-$(uname -r)
WORK=$(mktemp -d /var/tmp/schema-wdstall.XXXXXX); ROOT=$WORK/root
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$ROOT"/{sbin,bin,usr/bin,etc/schema-init/services,proc,sys,dev,run,tmp,sys/fs/cgroup}
cp "$REPO/schema-init" "$ROOT/sbin/schema-init"
# static: the initramfs has no libc
cc -static -O2 -std=c99 -D_GNU_SOURCE -I"$REPO" -o "$ROOT/usr/bin/schema-ctl" "$REPO/schema-ctl.c"
cc -static -O2 -std=c11 -D_GNU_SOURCE -o "$ROOT/usr/bin/staller" "$REPO/tests/livetest/wdstall-staller.c"
zstd -dq -o "$ROOT/i6300esb.ko" /lib/modules/$(uname -r)/kernel/drivers/watchdog/i6300esb.ko.zst
printf '#!/bin/sh\n/bin/busybox mount -t devtmpfs dev /dev\n/bin/busybox insmod /i6300esb.ko\nexec /sbin/schema-init\n' > "$ROOT/init"
chmod +x "$ROOT/init"
cp /usr/sbin/busybox "$ROOT/bin/busybox"
for a in sh ls cat sleep touch rm grep echo kill insmod date; do ln -sf /bin/busybox "$ROOT/bin/$a"; done
ln -sf /bin/busybox "$ROOT/usr/bin/touch"

svc() { cat > "$ROOT/etc/schema-init/services/$1.svc"; }
cat > "$ROOT/usr/bin/dmt.sh" <<'EOF'
#!/bin/sh
while [ ! -f /tmp/stoppet ]; do
    /usr/bin/schema-ctl pet dmt >/dev/null 2>&1
    sleep 1
done
sleep 100000
EOF
svc dmt <<'EOF'
name=dmt
exec=/usr/bin/dmt.sh
watchdog_timeout_ms=3000
needs_root=1
EOF
cat > "$ROOT/usr/bin/drive.sh" <<'EOF'
#!/bin/sh
exec >/dev/console 2>&1
sleep 8
/usr/bin/staller stuck /run/schema-init.sock 75 &
/usr/bin/staller drip  /run/schema-init.sock 75 &
echo "STALL-START: stuck reader + dripping sender on the ctl socket for 75s"
i=0
while [ $i -lt 15 ]; do
    sleep 5; i=$((i+1))
    /usr/bin/schema-ctl status >/dev/null 2>&1 && ok=ok || ok=ERR
    echo "TICK-$i: status $ok"
done
echo "STALL-OK: 75s of stalled clients, PID 1 kept answering and the box is still up"
echo "DMT-STOP: pets end now, the device must fire within its 30s window"
touch /tmp/stoppet
sleep 60
echo "WD-NEVER-FIRED: the watchdog was not armed, the stall half proves nothing"
EOF
chmod +x "$ROOT"/usr/bin/*.sh
svc driver <<'EOF'
name=driver
exec=/usr/bin/drive.sh
needs_root=1
EOF

( cd "$ROOT" && find . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -9 ) > "$WORK/i.gz"
timeout 220 qemu-system-x86_64 -enable-kvm -m 512 -smp 2 -no-reboot -nographic -nic none \
  -kernel "$KERNEL" -initrd "$WORK/i.gz" \
  -device i6300esb -append "console=ttyS0 rdinit=/init panic=1 loglevel=4" </dev/null >"$OUT" 2>&1 || true

grep -E 'opened hardware watchdog|STALL-START|TICK-(5|10|15)|STALL-OK|DMT-STOP|WD-NEVER-FIRED|missed pet window|Kernel panic' "$OUT" || true
r=0
grep -q 'opened hardware watchdog' "$OUT" || { echo ">> MISS: engine never opened the watchdog"; r=1; }
grep -q 'STALL-OK' "$OUT" || { echo ">> MISS: reset during the stall — pets did not keep flowing"; r=1; }
grep -q 'DMT-STOP' "$OUT" || { echo ">> MISS: the DMT phase never started"; r=1; }
grep -q 'WD-NEVER-FIRED' "$OUT" && { echo ">> MISS: device never fired after pets stopped"; r=1; }
bad=$(grep -c 'TICK-.*ERR' "$OUT") || true
[ "$bad" -eq 0 ] || { echo ">> MISS: $bad status calls failed during the stall"; r=1; }
[ $r -eq 0 ] && echo ">> RESULT: PASS (pets survive a ctl stall; the armed device fires when pets stop)" \
             || echo ">> RESULT: FAIL (serial: $OUT)"
exit $r
