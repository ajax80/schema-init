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

# Minimal passwd/group so node_apply_perms can resolve GROUP= names (getgrnam);
# without these the baseline video/disk/input group grants silently no-op.
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
disk:x:6:
video:x:39:
audio:x:63:
input:x:104:
render:x:105:
EOF
cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
EOF

# Gap-6 probe: a group-2 (udev monitor) listener. schema-udev broadcasts every
# processed event during coldplug, so a listener bound BEFORE it starts must
# receive at least one libudev frame -- the exact channel kwin/libinput use and
# the one whose absence killed the 08-14 flip.
cat > "$WORK/monprobe.c" <<'EOF'
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/netlink.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
int main(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) { printf("MONPROBE: NOSOCK\n"); return 2; }
    struct sockaddr_nl sa; memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK; sa.nl_groups = 2;   /* UDEV_MONITOR_UDEV */
    if (bind(fd, (void *)&sa, sizeof sa) < 0) { printf("MONPROBE: NOBIND\n"); return 2; }
    struct timeval tv = { 20, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[8192];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) { printf("MONPROBE: NONE\n"); return 1; }
        if (n > 8 && memcmp(buf, "libudev", 7) == 0) { printf("MONPROBE: GOT %zd\n", n); return 0; }
    }
}
EOF
cc -static -O2 -o "$ROOT/bin/monprobe" "$WORK/monprobe.c" 2>/dev/null || {
  cc -O2 -o "$ROOT/bin/monprobe" "$WORK/monprobe.c"   # dynamic: libc/loader already bundled
  for lib in $(ldd "$ROOT/bin/monprobe" | grep -oE '/[^ ]+\.so[^ ]*'); do
    cp -Ln "$lib" "$ROOT/lib64/$(basename "$lib")" 2>/dev/null || true
  done
}

cat > "$ROOT/init" <<'INIT'
#!/bin/busybox sh
export PATH=/bin:/sbin
busybox mount -t proc proc /proc
busybox mount -t sysfs sys /sys
busybox mount -t devtmpfs dev /dev
busybox mkdir -p /run/udev/data /dev/disk
echo "UDEV-VMTEST: booting schema-udev in LIVE mode"
# Gap 6: bind the udev monitor group BEFORE schema-udev so coldplug broadcasts
# are caught. Runs to a file; we read the verdict after coldplug settles.
/bin/monprobe > /run/monprobe.out 2>&1 &
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

# Gap 3: /dev/{char,block} major:minor symlink farm must be populated.
CH=$(busybox ls /dev/char 2>/dev/null | busybox grep -c .)
BL=$(busybox ls /dev/block 2>/dev/null | busybox grep -c .)
echo "UDEV-VMTEST: farm char=$CH block=$BL"
echo "== /dev/block sample =="; busybox ls -l /dev/block 2>&1 | busybox head -4

# Gap 5: /run/udev/data/* records must be world-readable (0644), not 0600.
DATA_BAD=0; DATA_N=0
for f in /run/udev/data/*; do
  [ -e "$f" ] || continue
  DATA_N=$((DATA_N+1))
  m=$(busybox stat -c '%a' "$f")
  [ "$m" = "644" ] || { DATA_BAD=$((DATA_BAD+1)); echo "UDEV-VMTEST: bad mode $m on $f"; }
done
echo "UDEV-VMTEST: data files=$DATA_N badmode=$DATA_BAD"

# Gap 4: block device nodes must carry the baseline disk group + 0660, not
# root:root 0600 (the GPT disk vda + its partitions).
NODE_OK=0
for n in /dev/vda /dev/vda1; do
  [ -b "$n" ] || continue
  gm="$(busybox stat -c '%G %a' "$n")"
  echo "UDEV-VMTEST: node $n -> $gm"
  case "$gm" in "disk 660") NODE_OK=$((NODE_OK+1));; esac
done

# Gap 6: the monitor probe must have caught a broadcast frame during coldplug.
MON="$(busybox cat /run/monprobe.out 2>/dev/null)"
echo "UDEV-VMTEST: monitor $MON"

FAIL=0
[ "$PU" -ge 3 ] && [ "$UU" -ge 3 ] && [ "$LB" -ge 3 ] || FAIL=1
[ "$CH" -ge 1 ] && [ "$BL" -ge 1 ] || FAIL=1
[ "$DATA_N" -ge 1 ] && [ "$DATA_BAD" -eq 0 ] || FAIL=1
[ "$NODE_OK" -ge 1 ] || FAIL=1
case "$MON" in *"MONPROBE: GOT"*) ;; *) FAIL=1;; esac
if [ "$FAIL" -eq 0 ]; then
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
