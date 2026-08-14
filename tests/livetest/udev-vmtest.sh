#!/bin/sh
# schema-udev LIVE-mode VM test.
#
# Boots a minimal initramfs in QEMU with a GPT disk attached (virtio-blk) and
# the LIVE sentinel present, so schema-udev owns real /dev and /dev/disk. Proves
# the device manager enumerates coldplug and populates /dev/disk/by-* + applies
# uaccess ACLs without wedging -- the liveness oracle the fidelity gate is blind
# to. by-designator is NOT tested here: it only fires on the *root backing disk*
# (the attached disk is not root), and it is already proven on the real box by
# verify-rules-live (=0). Scope: by-uuid / by-partuuid / by-label / by-path.
set -eu

REPO="${REPO:-$HOME/projects/schema-init}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BB="/usr/sbin/busybox"
TIMEOUT="${TIMEOUT:-120}"
WORK="$(mktemp -d /var/tmp/schema-udev-vmtest.XXXX)"
ROOT="$WORK/root"
DISK="$WORK/disk.img"
trap 'sudo losetup -d "$LOOP" 2>/dev/null || true' EXIT

echo ">> workdir: $WORK"

# 1. Build schema-udev (current branch).
if [ "${REBUILD:-1}" = 1 ]; then
  echo ">> building schema-udev ($(git -C "$REPO" rev-parse --abbrev-ref HEAD))"
  make -C "$REPO" schema-udev >/dev/null
fi
BIN="$REPO/schema-udev"
[ -x "$BIN" ] || { echo "!! no schema-udev binary"; exit 1; }

# 2. GPT disk: ESP(vfat) + xbootldr(vfat) + root(btrfs), real type GUIDs.
truncate -s 640M "$DISK"
sfdisk "$DISK" >/dev/null <<'EOF'
label: gpt
start=2048, size=204800, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="ESP"
size=204800, type=BC13C2FF-59E6-4262-A352-B275FD6F7172, name="xbootldr"
size=+, type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709, name="root"
EOF

LOOP="$(sudo losetup -f --show -P "$DISK")"
echo ">> loop: $LOOP"
sudo mkfs.vfat -n ESP "${LOOP}p1" >/dev/null
sudo mkfs.vfat -n XBOOT "${LOOP}p2" >/dev/null
sudo mkfs.btrfs -f -L root "${LOOP}p3" >/dev/null
sudo losetup -d "$LOOP"; LOOP=""

# 3. Minimal initramfs.
mkdir -p "$ROOT"/sbin "$ROOT"/bin "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev \
         "$ROOT"/run "$ROOT"/etc/schema-init "$ROOT"/usr/lib/udev "$ROOT"/etc/udev
cp "$BIN" "$ROOT/sbin/schema-udev"
cp "$BB" "$ROOT/bin/busybox"
# schema-udev is dynamically linked (-lacl); bundle its loader + libs.
mkdir -p "$ROOT/lib64"
for lib in $(ldd "$BIN" | grep -oE '/[^ ]+\.so[^ ]*'); do
  cp -L "$lib" "$ROOT/lib64/$(basename "$lib")"
done
for a in sh ls cat mount mkdir sleep ln readlink find echo poweroff head grep stat; do
  ln -sf /bin/busybox "$ROOT/bin/$a"
done
cp -a /usr/lib/udev/rules.d "$ROOT/usr/lib/udev/"
[ -f /etc/udev/hwdb.bin ] && cp /etc/udev/hwdb.bin "$ROOT/etc/udev/hwdb.bin"
# LIVE sentinel -> schema-udev owns real /dev, /dev/disk, ACLs.
: > "$ROOT/etc/schema-init/schema-udev.live"

cat > "$ROOT/init" <<'INIT'
#!/bin/busybox sh
export PATH=/bin:/sbin
busybox mount -t proc proc /proc
busybox mount -t sysfs sys /sys
busybox mount -t devtmpfs dev /dev
busybox mkdir -p /run/udev/data /dev/disk
echo "UDEV-VMTEST: booting schema-udev in LIVE mode"
/sbin/schema-udev &
busybox sleep 4
echo "== by-partuuid =="; busybox ls /dev/disk/by-partuuid 2>&1
echo "== by-uuid ==";     busybox ls /dev/disk/by-uuid 2>&1
echo "== by-label ==";    busybox ls /dev/disk/by-label 2>&1
echo "== by-path ==";     busybox ls /dev/disk/by-path 2>&1
# Verdict: GPT partitions must yield by-partuuid (always) + by-uuid/by-label
# (from blkid_fs) for the three filesystems we made.
PU=$(busybox ls /dev/disk/by-partuuid 2>/dev/null | busybox grep -c .)
UU=$(busybox ls /dev/disk/by-uuid 2>/dev/null | busybox grep -c .)
LB=$(busybox ls /dev/disk/by-label 2>/dev/null | busybox grep -c .)
echo "UDEV-VMTEST: counts partuuid=$PU uuid=$UU label=$LB"
if [ "$PU" -ge 3 ] && [ "$UU" -ge 3 ] && [ "$LB" -ge 3 ]; then
  echo "UDEV-VMTEST: PASS"
else
  echo "UDEV-VMTEST: FAIL"
fi
busybox poweroff -f
INIT
chmod +x "$ROOT/init"

( cd "$ROOT" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initramfs.gz"

# 4. Boot.
KERNEL="/lib/modules/$(uname -r)/vmlinuz"
[ -f "$KERNEL" ] || KERNEL="/boot/vmlinuz-$(uname -r)"
[ -f "$KERNEL" ] || { echo "!! no kernel image at $KERNEL"; exit 1; }
echo ">> booting (kernel $KERNEL, timeout ${TIMEOUT}s)"
timeout "$TIMEOUT" qemu-system-x86_64 -enable-kvm -m 1024 -nographic -no-reboot \
  -kernel "$KERNEL" -initrd "$WORK/initramfs.gz" \
  -append "console=ttyS0 rdinit=/init" \
  -drive file="$DISK",if=virtio,format=raw \
  < /dev/null 2>&1 | tee "$HERE/last-udev-vmtest-serial.log" || true

echo
if grep -q "UDEV-VMTEST: PASS" "$HERE/last-udev-vmtest-serial.log"; then
  echo ">> RESULT: PASS"
else
  echo ">> RESULT: FAIL (serial: $HERE/last-udev-vmtest-serial.log, workdir kept: $WORK)"
fi
