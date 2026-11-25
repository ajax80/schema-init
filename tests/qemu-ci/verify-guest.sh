#!/bin/sh
# CI stage 4b: schema-ctl verify against a real boot layout, from inside the box it
# judges. OVMF boots GRUB (blscfg) off a FAT ESP; the BLS entry boots a tiny
# initramfs that mounts the ext4 root and switch_roots into schema-init, as dracut
# would. In that guest verify runs on the live /boot/grub2 + /boot/loader/entries:
#   clean         the next boot as it is: no FAIL
#   missing init  the next boot's init= names a file not on its root: FAIL
#   lost service  a baselined .svc the next boot would refuse (group-writable): FAIL
# The unit tests judge fixtures; this is the same code on a mounted root, real grubenv,
# real grub.cfg and a real --check run by the next boot's init.
set -eu
cd "$(dirname "$0")/../.."
KERNEL=${KERNEL:-/boot/vmlinuz-$(uname -r)}
# /lib/modules/<v>/vmlinuz (Fedora's kernel-core) or /boot/vmlinuz-<v>
case $KERNEL in /lib/modules/*/vmlinuz) KMOD=${KMOD:-$(dirname "$KERNEL")} ;; *) KMOD=${KMOD:-/lib/modules/$(basename "$KERNEL" | sed 's/^vmlinuz-//')} ;; esac
BB=${BUSYBOX:-$(command -v busybox)}
GRUBDIR=${GRUBDIR:-/usr/lib/grub/x86_64-efi}
OVMF=${OVMF:-/usr/share/edk2/ovmf}
TIMEOUT=${TIMEOUT:-240}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for t in grub2-mkstandalone sfdisk mkfs.vfat mkfs.ext4 fakeroot mcopy mmd qemu-system-x86_64; do
    command -v $t >/dev/null || { echo "FAIL: $t missing"; exit 1; }
done
[ -r "$KERNEL" ] || { echo "FAIL: kernel $KERNEL not readable"; exit 1; }
static() { file -L "$1" | grep -Eq 'static(ally|-pie) linked'; }
static "$BB" || { echo "FAIL: $BB is not a static busybox"; exit 1; }
make -s schema-init
static schema-init || { echo "FAIL: ./schema-init is not static; make clean first"; exit 1; }
SCTL_SRCS=$(make -s --no-print-directory -f Makefile -f - print-sctl-srcs <<'MK'
print-sctl-srcs:
	@echo $(SCHEMA_CTL_SRCS)
MK
)
# shellcheck disable=SC2086
cc -static -O2 -std=c99 -D_GNU_SOURCE -I. -DSCHEMA_INIT_VERSION='"ci"' -o "$WORK/schema-ctl" $SCTL_SRCS -lrt

OPTS="console=ttyS0 root=/dev/vda2 rw init=/sbin/schema-init panic=1 loglevel=4"

# initramfs: the disk driver if it's a module, mount root, hand over to init= (busybox switch_root)
I=$WORK/initrd
mkdir -p "$I"/bin "$I"/dev "$I"/proc "$I"/newroot "$I"/lib
cp "$BB" "$I/bin/busybox"
for a in sh mount umount insmod sleep switch_root cat grep sed; do ln -s busybox "$I/bin/$a"; done
for m in virtio_blk; do
    f=$(find "$KMOD" -name "$m.ko*" 2>/dev/null | head -1)
    [ -n "$f" ] || continue
    case $f in *.zst) zstd -qdc "$f" ;; *.xz) xz -dc "$f" ;; *.gz) gzip -dc "$f" ;; *) cat "$f" ;; esac > "$I/lib/$m.ko"
done
cat > "$I/init" <<'EOF'
#!/bin/sh
mount -t devtmpfs dev /dev
mount -t proc proc /proc
for m in /lib/*.ko; do [ -e "$m" ] && insmod "$m"; done
i=0; while [ ! -b /dev/vda2 ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
mount -t ext4 -o rw /dev/vda2 /newroot || { echo "CI-INITRD: cannot mount root"; exec sh; }
init=$(sed -n 's/.*init=\([^ ]*\).*/\1/p' /proc/cmdline)
umount /proc /dev
exec switch_root /newroot "${init:-/sbin/schema-init}"
EOF
chmod +x "$I/init"
( cd "$I" && find . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -1 ) > "$WORK/initramfs.img"

# the root: schema-init, schema-ctl, its services, and the /boot verify reads
R=$WORK/root
mkdir -p "$R"/bin "$R"/sbin "$R"/etc/schema-init/services "$R"/dev "$R"/proc "$R"/sys "$R"/run "$R"/tmp \
         "$R"/var/lib "$R"/boot/grub2 "$R"/boot/loader/entries
cp schema-init "$R/sbin/schema-init"
cp "$WORK/schema-ctl" "$R/bin/schema-ctl"
cp "$BB" "$R/bin/busybox"
for a in sh cat echo grep sed sleep poweroff rm mv chmod; do ln -s busybox "$R/bin/$a"; done
printf '/dev/vda2 / ext4 rw 0 1\n' > "$R/etc/fstab"
cp "$KERNEL" "$R/boot/vmlinuz-ci"
cp "$WORK/initramfs.img" "$R/boot/initramfs-ci.img"
E=$R/boot/loader/entries/ci-next.conf
printf 'title ci next\nversion ci\nlinux /boot/vmlinuz-ci\ninitrd /boot/initramfs-ci.img\noptions %s\n' "$OPTS" > "$E"
{
    printf 'serial --unit=0 --speed=115200\nterminal_input serial console\nterminal_output serial console\nset timeout=0\n'
    printf 'load_env -f ($root)/boot/grub2/grubenv\nset default="${saved_entry}"\n'
    printf 'function load_video { true; }\ninsmod blscfg\nblscfg\n'
} > "$R/boot/grub2/grub.cfg"
{ printf '# GRUB Environment Block\nsaved_entry=ci-next\n'; head -c 1024 /dev/zero | tr '\0' '#'; } | head -c 1024 > "$R/boot/grub2/grubenv"
printf 'name=ci-idle\nexec=/bin/sleep\nargs=100000\n' > "$R/etc/schema-init/services/ci-idle.svc"

cat > "$R/bin/ci-verify" <<'EOF'
#!/bin/sh
( sleep 150; echo CI-WEDGED > /dev/console; poweroff -f ) &
v() {
    echo "CI-VERIFY-BEGIN $1" > /dev/console
    /bin/schema-ctl verify > /dev/console 2>&1; rc=$?
    echo "CI-VERIFY-END $1 rc=$rc" > /dev/console
}
/bin/schema-ctl verify --write-baseline --wait-settled 20 > /dev/console 2>&1
echo "CI-BASELINE rc=$?" > /dev/console
v clean
E=/boot/loader/entries/ci-next.conf
sed 's|init=/sbin/schema-init|init=/sbin/ci-no-such-init|' $E > /tmp/e && cat /tmp/e > $E
v missing-init
sed 's|init=/sbin/ci-no-such-init|init=/sbin/schema-init|' $E > /tmp/e && cat /tmp/e > $E
chmod g+w /etc/schema-init/services/ci-idle.svc
v lost-service
chmod g-w /etc/schema-init/services/ci-idle.svc
/bin/schema-ctl poweroff > /dev/console 2>&1
EOF
chmod +x "$R/bin/ci-verify"
printf 'name=ci-verify\nexec=/bin/ci-verify\noneshot=1\ndep=ci-idle\n' > "$R/etc/schema-init/services/ci-verify.svc"

# root-owned, as the boot needs its .svc files
fakeroot sh -c "chown -R 0:0 '$R' && mkfs.ext4 -q -F -L ci-root -d '$R' '$WORK/root.img' 320M"

# GPT: ESP (GRUB, finds /boot/grub2/grub.cfg on the root) + the ext4 root
D=$WORK/disk.img
truncate -s 400M "$D"
printf 'label: gpt\nstart=2048, size=60M, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B\nsize=330M, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4\n' | sfdisk -q "$D"
# shellcheck disable=SC2046
set -- $(sfdisk -q -l -o Start,Sectors "$D" | sed -n '2,3p')
P1S=$1 P1N=$2 P2S=$3
mkfs.vfat --offset "$P1S" "$D" $(( P1N / 2 )) >/dev/null 2>&1
dd if="$WORK/root.img" of="$D" bs=512 seek="$P2S" conv=notrunc status=none
printf 'search --no-floppy --file /boot/grub2/grub.cfg --set=root\nconfigfile ($root)/boot/grub2/grub.cfg\n' > "$WORK/embed.cfg"
grub2-mkstandalone -d "$GRUBDIR" -O x86_64-efi -o "$WORK/BOOTX64.EFI" \
    --modules="part_gpt fat ext2 search search_fs_file configfile loadenv blscfg linux echo test serial terminal gzio all_video" \
    "boot/grub/grub.cfg=$WORK/embed.cfg"
M="-i $D@@$(( P1S * 512 ))"
mmd $M ::/EFI ::/EFI/BOOT
mcopy $M "$WORK/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$OVMF/OVMF_VARS.fd" "$WORK/vars.fd"

ACCEL=tcg CPU=max
[ -w /dev/kvm ] && ACCEL=kvm CPU=host
[ "$ACCEL" = tcg ] && TIMEOUT=${TIMEOUT_TCG:-600}
echo ">> booting ($ACCEL)"
rc=0
timeout "$TIMEOUT" qemu-system-x86_64 -accel "$ACCEL" -cpu "$CPU" -machine q35 -m 768 -nographic -nic none \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF/OVMF_CODE.fd" \
    -drive if=pflash,format=raw,file="$WORK/vars.fd" \
    -drive file="$D",format=raw,if=virtio \
    < /dev/null > "$WORK/serial.log" 2>&1 || rc=$?
L=$WORK/serial.txt
tr -d '\r' < "$WORK/serial.log" > "$L"
sec() { sed -n "/CI-VERIFY-BEGIN $1\$/,/CI-VERIFY-END $1 /p" "$L"; }

pass=1
miss() { echo "  MISS: $1"; pass=0; }
[ "$rc" = 0 ] || miss "qemu exited $rc"
grep -q 'CI-BASELINE rc=0' "$L" || miss "verify --write-baseline failed"
sec clean | grep -q 'PASS\] actual next boot ci-next' || miss "clean: next boot ci-next not PASS"
sec clean | grep -q 'services match the baseline' || miss "clean: services did not match the baseline"
sec clean | grep -q '^\[FAIL\]' && miss "clean: a FAIL"
sec missing-init | grep -q 'FAIL\] .*ci-next.*init=/sbin/ci-no-such-init does not exist in the target' || miss "missing-init: not a FAIL"
sec missing-init | grep -q 'CI-VERIFY-END missing-init rc=1' || miss "missing-init: verify did not exit 1"
sec lost-service | grep -Eq 'FAIL\].*ci-idle' || miss "lost-service: ci-idle not a FAIL"
sec lost-service | grep -q 'CI-VERIFY-END lost-service rc=1' || miss "lost-service: verify did not exit 1"
grep -q 'PID 1 poweroff' "$L" || miss "no clean poweroff"
grep -q CI-WEDGED "$L" && miss "wedged, forced off"

if [ "$pass" = 1 ]; then
    echo ">> PASS: verify, in the guest, passed the real next boot and failed a missing init= and a service the next boot would lose"
else
    echo ">> FAIL — verify sections:"
    sed -n '/CI-BASELINE/,$p' "$L" | grep -v '^\[ *[0-9.]*\]' | head -120
    grep -aE 'CI-INITRD|panic|error' "$L" | head
    exit 1
fi
