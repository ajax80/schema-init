#!/bin/sh
# schema-udev BOOT-INTEGRATION test: the real flip wiring, end to end.
#
# Boots schema-init as PID 1 in QEMU with the exact post-flip service layout:
#   udevd.svc   -> exec=/usr/bin/schema-udev, ready_path=/run/schema-udev/ready
#   net-test.svc-> dep=udevd  (stands in for network-up.svc: proves a dep on the
#                  device-manager service resolves only AFTER schema-udev signals
#                  ready -- the readiness handshake that the naive flip lacked)
#   verdict.svc -> dep=net-test, dumps result to console + powers off
# LIVE sentinel present, GPT disk attached. Proves: schema-init brings up
# schema-udev as the device manager, schema-udev owns /dev + writes the ready
# marker after coldplug, and dep=udevd services then fire. This is the test that
# makes the reboot-flip safe (vs udev-vmtest.sh which proves device mgmt alone).
set -eu

REPO="${REPO:-$HOME/projects/schema-init}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BB="/usr/sbin/busybox"
TIMEOUT="${TIMEOUT:-120}"
WORK="$(mktemp -d /var/tmp/schema-udev-boot.XXXX)"
ROOT="$WORK/root"
DISK="$WORK/disk.img"
trap 'sudo losetup -d "$LOOP" 2>/dev/null || true' EXIT

echo ">> workdir: $WORK"

if [ "${REBUILD:-1}" = 1 ]; then
  echo ">> building schema-init + schema-udev ($(git -C "$REPO" rev-parse --abbrev-ref HEAD))"
  make -C "$REPO" schema-init schema-udev >/dev/null
fi
INIT="$REPO/schema-init"; UDEV="$REPO/schema-udev"
[ -x "$INIT" ] && [ -x "$UDEV" ] || { echo "!! missing binaries"; exit 1; }

# GPT disk: ESP(vfat)+xbootldr(vfat)+root(btrfs), real type GUIDs.
truncate -s 640M "$DISK"
sfdisk "$DISK" >/dev/null <<'EOF'
label: gpt
start=2048, size=204800, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="ESP"
size=204800, type=BC13C2FF-59E6-4262-A352-B275FD6F7172, name="xbootldr"
size=+, type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709, name="root"
EOF
LOOP="$(sudo losetup -f --show -P "$DISK")"
sudo mkfs.vfat -n ESP "${LOOP}p1" >/dev/null
sudo mkfs.vfat -n XBOOT "${LOOP}p2" >/dev/null
sudo mkfs.btrfs -f -L root "${LOOP}p3" >/dev/null
sudo losetup -d "$LOOP"; LOOP=""

# initramfs
mkdir -p "$ROOT"/sbin "$ROOT"/bin "$ROOT"/usr/bin "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev \
         "$ROOT"/run "$ROOT"/lib64 "$ROOT"/etc/schema-init/services \
         "$ROOT"/usr/lib/udev "$ROOT"/etc/udev "$ROOT"/var/log/schema-init
cp "$INIT" "$ROOT/sbin/schema-init"
ln -sf /sbin/schema-init "$ROOT/init"        # kernel initramfs entry point
cp "$UDEV" "$ROOT/usr/bin/schema-udev"
for lib in $(ldd "$UDEV" | grep -oE '/[^ ]+\.so[^ ]*'); do
  cp -L "$lib" "$ROOT/lib64/$(basename "$lib")"
done
cp "$BB" "$ROOT/bin/busybox"
for a in sh ls cat mount mkdir sleep ln readlink find echo poweroff head grep stat touch; do
  ln -sf /bin/busybox "$ROOT/bin/$a"
done
ln -sf /bin/busybox "$ROOT/usr/bin/touch"
cp -a /usr/lib/udev/rules.d "$ROOT/usr/lib/udev/"
[ -f /etc/udev/hwdb.bin ] && cp /etc/udev/hwdb.bin "$ROOT/etc/udev/hwdb.bin"
: > "$ROOT/etc/schema-init/schema-udev.live"     # LIVE sentinel

# --- the post-flip service wiring ---
cat > "$ROOT/etc/schema-init/services/udevd.svc" <<'EOF'
name=udevd
exec=/usr/bin/schema-udev
needs_root=1
critical=1
priority=critical
ready_path=/run/schema-udev/ready
EOF
cat > "$ROOT/etc/schema-init/services/net-test.svc" <<'EOF'
name=net-test
exec=/bin/touch
args=/run/net-test-ran
oneshot=1
dep=udevd
EOF
cat > "$ROOT/etc/schema-init/services/verdict.svc" <<'EOF'
name=verdict
exec=/usr/bin/vmverdict.sh
oneshot=1
dep=net-test
EOF

cat > "$ROOT/usr/bin/vmverdict.sh" <<'EOF'
#!/bin/sh
exec >/dev/console 2>&1
echo "===== UDEV-BOOT-REPORT ====="
[ -e /run/schema-udev/ready ] && echo "ready-marker: present" || echo "ready-marker: MISSING"
[ -e /run/net-test-ran ]      && echo "dep=udevd svc ran: yes" || echo "dep=udevd svc ran: NO"
PU=$(busybox ls /dev/disk/by-partuuid 2>/dev/null | busybox grep -c .)
UU=$(busybox ls /dev/disk/by-uuid 2>/dev/null | busybox grep -c .)
echo "by-partuuid=$PU by-uuid=$UU"
if [ -e /run/schema-udev/ready ] && [ -e /run/net-test-ran ] && [ "$PU" -ge 3 ] && [ "$UU" -ge 3 ]; then
  echo "UDEV-BOOT: PASS"
else
  echo "UDEV-BOOT: FAIL"
fi
busybox sync
busybox poweroff -f
EOF
chmod +x "$ROOT/usr/bin/vmverdict.sh"

( cd "$ROOT" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initramfs.gz"

KERNEL="/lib/modules/$(uname -r)/vmlinuz"
[ -f "$KERNEL" ] || KERNEL="/boot/vmlinuz-$(uname -r)"
[ -f "$KERNEL" ] || { echo "!! no kernel image"; exit 1; }
echo ">> booting schema-init as PID 1 (timeout ${TIMEOUT}s)"
timeout "$TIMEOUT" qemu-system-x86_64 -enable-kvm -m 1024 -nographic -no-reboot \
  -kernel "$KERNEL" -initrd "$WORK/initramfs.gz" \
  -append "console=ttyS0 rdinit=/sbin/schema-init panic=1 loglevel=4" \
  -drive file="$DISK",if=virtio,format=raw \
  < /dev/null 2>&1 | tee "$HERE/last-udev-boot-serial.log" || true

echo
if grep -q "UDEV-BOOT: PASS" "$HERE/last-udev-boot-serial.log"; then
  echo ">> RESULT: PASS  (schema-init -> schema-udev live -> ready -> dep=udevd svc fired)"
else
  echo ">> RESULT: FAIL  (serial: $HERE/last-udev-boot-serial.log, workdir: $WORK)"
fi
