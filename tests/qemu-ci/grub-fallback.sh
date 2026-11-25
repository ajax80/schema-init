#!/bin/sh
# CI stage 4a: the boot-success guard's fallback under REAL GRUB. Until now only a
# model of GRUB's shell checked it (tests/test_bootguard_logic.py): a direct
# kernel boot never runs GRUB. Here OVMF boots Fedora's GRUB (blscfg) from a FAT
# ESP with a writable grubenv; grub.cfg is the stock 08_fallback_counting and
# 10_reset_boot_success around our scripts/09_schema_fallback, BLS entries pick
# the kernel. No root: the disk is built with sfdisk, mkfs.vfat and mtools.
#   armed:   default entry won't boot, boot_counter=2 (schema-snapshot arm's default)
#            -> boot 1 counts 2->1 and panics in the default, boot 2 counts 1->0 and
#            must be the schema_fallback_id entry, booting schema-init. 08 counts down
#            before 09 looks, so N gives the new entry N-1 tries
#   unarmed: no boot_counter -> the default entry boots, never the fallback
set -eu
cd "$(dirname "$0")/../.."
KERNEL=${KERNEL:-/boot/vmlinuz-$(uname -r)}
BB=${BUSYBOX:-$(command -v busybox)}
GRUBDIR=${GRUBDIR:-/usr/lib/grub/x86_64-efi}
OVMF=${OVMF:-/usr/share/edk2/ovmf}
TIMEOUT=${TIMEOUT:-180}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for t in grub2-mkstandalone sfdisk mkfs.vfat mcopy mmd qemu-system-x86_64; do
    command -v $t >/dev/null || { echo "FAIL: $t missing"; exit 1; }
done
[ -e "$GRUBDIR/blscfg.mod" ] || { echo "FAIL: no blscfg.mod in $GRUBDIR (Fedora grub2-efi-x64-modules)"; exit 1; }
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

# the entry that boots: schema-init as PID 1 says which BLS entry it came from, then powers off
R=$WORK/root
mkdir -p "$R"/bin "$R"/sbin "$R"/etc/schema-init/services "$R"/dev "$R"/proc "$R"/sys "$R"/run "$R"/tmp
cp schema-init "$R/sbin/schema-init"
cp "$WORK/schema-ctl" "$R/bin/schema-ctl"
cp "$BB" "$R/bin/busybox"
for a in sh cat echo grep sleep poweroff; do ln -s busybox "$R/bin/$a"; done
printf '#!/bin/sh\necho "CI-BOOTED $(grep -o "ci_entry=[a-z]*" /proc/cmdline)" > /dev/console\n( sleep 60; poweroff -f ) &\n/bin/schema-ctl poweroff > /dev/console 2>&1\n' > "$R/bin/ci-report"
chmod +x "$R/bin/ci-report"
printf 'name=ci-report\nexec=/bin/ci-report\noneshot=1\n' > "$R/etc/schema-init/services/ci-report.svc"
( cd "$R" && find . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -1 ) > "$WORK/good.gz"
# no init at all: the kernel panics, panic=1 reboots into GRUB again
mkdir "$WORK/empty" && ( cd "$WORK/empty" && echo . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -1 ) > "$WORK/broken.gz"

# grub.cfg as grub2-mkconfig orders it: header (load_env, default=saved_entry), 08, 09, 10, blscfg.
# The stock hooks from this box's /etc/grub.d when readable, so the test tracks what boots
hook() {
    if [ -r "/etc/grub.d/$1" ] && sh "/etc/grub.d/$1" > /dev/null 2>&1; then sh "/etc/grub.d/$1"
    else sed -n "/^## $1\$/,/^## end\$/p" tests/qemu-ci/grub-stock-hooks | sed '1d;$d'; fi
}
{
    printf 'serial --unit=0 --speed=115200\nterminal_input serial console\nterminal_output serial console\nset timeout=0\n'
    # env_block, as Fedora's header does for a grubenv outside $prefix (the read-only memdisk here)
    printf 'set env_block=($root)/grubenv\nload_env -f "${env_block}"\nset default="${saved_entry}"\n'
    hook 08_fallback_counting
    sh scripts/09_schema_fallback
    hook 10_reset_boot_success
    # the decision, on the serial log
    printf 'echo "CI-GRUB counter=${boot_counter} success=${boot_success} default=${default}"\n'
    # Fedora's blscfg entries call load_video, which 00_header defines
    printf 'function load_video { true; }\ninsmod blscfg\nblscfg\n'
} > "$WORK/grub.cfg"
printf 'search --no-floppy --file /ci-esp --set=root\nconfigfile ($root)/grub.cfg\n' > "$WORK/embed.cfg"
grub2-mkstandalone -d "$GRUBDIR" -O x86_64-efi -o "$WORK/BOOTX64.EFI" \
    --modules="part_gpt fat search search_fs_file configfile loadenv blscfg linux echo test increment serial terminal gzio all_video" \
    "boot/grub/grub.cfg=$WORK/embed.cfg"

entry() {
    printf 'title %s\nversion %s\nlinux /vmlinuz\ninitrd /%s\noptions console=ttyS0 rdinit=/sbin/schema-init panic=1 loglevel=4 ci_entry=%s\n' \
        "$1" "$2" "$3" "$1"
}

# $1 name; the rest grubenv KEY=VALUE pairs
run() {
    name=$1; shift
    D=$WORK/$name.img
    truncate -s 96M "$D"
    printf 'label: gpt\nstart=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B\n' | sfdisk -q "$D"
    mkfs.vfat -F 32 --offset 2048 "$D" 97280 >/dev/null
    M="-i $D@@1M"
    mmd $M ::/EFI ::/EFI/BOOT ::/loader ::/loader/entries
    mcopy $M "$WORK/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
    mcopy $M "$WORK/grub.cfg" ::/grub.cfg
    mcopy $M "$KERNEL" ::/vmlinuz
    mcopy $M "$WORK/good.gz" ::/good.gz
    mcopy $M "$WORK/broken.gz" ::/broken.gz
    : > "$WORK/ci-esp"; mcopy $M "$WORK/ci-esp" ::/ci-esp
    entry broken 2 broken.gz > "$WORK/e"; mcopy $M "$WORK/e" ::/loader/entries/ci-broken.conf
    entry good 1 good.gz > "$WORK/e"; mcopy $M "$WORK/e" ::/loader/entries/ci-good.conf
    entry fallback 0 good.gz > "$WORK/e"; mcopy $M "$WORK/e" ::/loader/entries/ci-fallback.conf
    # a 1024-byte grubenv, as grub2-editenv writes it: save_env rewrites it in place
    { printf '# GRUB Environment Block\n'; for kv; do printf '%s\n' "$kv"; done; } > "$WORK/env"
    head -c 1024 /dev/zero | tr '\0' '#' | dd of="$WORK/env" bs=1 seek="$(wc -c < "$WORK/env")" conv=notrunc 2>/dev/null
    truncate -s 1024 "$WORK/env"
    mcopy $M "$WORK/env" ::/grubenv
    cp "$OVMF/OVMF_VARS.fd" "$WORK/vars.fd"
    ACCEL=tcg CPU=max
    [ -w /dev/kvm ] && ACCEL=kvm CPU=host
    echo ">> $name ($ACCEL)"
    rc=0
    timeout "$TIMEOUT" qemu-system-x86_64 -accel "$ACCEL" -cpu "$CPU" -machine q35 -m 512 -nographic -nic none \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF/OVMF_CODE.fd" \
        -drive if=pflash,format=raw,file="$WORK/vars.fd" \
        -drive file="$D",format=raw,if=virtio \
        < /dev/null > "$WORK/$name.log" 2>&1 || rc=$?
    tr -d '\r' < "$WORK/$name.log" > "$WORK/$name.txt"
    # the grubenv GRUB left behind
    mcopy $M ::/grubenv "$WORK/$name.env.out"
    return $rc
}

pass=1
miss() { echo "  MISS: $1"; pass=0; }

rc=0; run armed saved_entry=ci-broken boot_counter=2 boot_success=0 schema_fallback_id=ci-fallback || rc=$?
L=$WORK/armed.txt
[ "$rc" = 0 ] || miss "armed: qemu exited $rc"
grep -q 'CI-GRUB counter=1 success=0 default=ci-broken' "$L" || miss "armed: boot 1 did not count down and try the default"
grep -q 'CI-GRUB counter=0 success=0 default=ci-fallback' "$L" || miss "armed: boot 2 did not pick schema_fallback_id"
[ "$(grep -c 'Kernel panic' "$L")" = 1 ] || miss "armed: expected exactly one panic (the broken default)"
grep -q 'CI-BOOTED ci_entry=fallback' "$L" || miss "armed: the fallback entry did not boot schema-init"
grep -q 'PID 1 poweroff' "$L" || miss "armed: the fallback boot did not power off cleanly"
# save_env really wrote the FAT grubenv: the countdown survived the reboot
grep -q '^boot_counter=0' "$WORK/armed.env.out" || miss "armed: grubenv boot_counter is not 0 after the fallback"

rc=0; run unarmed saved_entry=ci-good schema_fallback_id=ci-fallback || rc=$?
L=$WORK/unarmed.txt
[ "$rc" = 0 ] || miss "unarmed: qemu exited $rc"
grep -q 'CI-GRUB counter= success=0 default=ci-good' "$L" || miss "unarmed: GRUB did not keep the default"
grep -q 'CI-BOOTED ci_entry=good' "$L" || miss "unarmed: the default entry did not boot"
grep -q 'ci_entry=fallback' "$L" && miss "unarmed: the fallback booted with no boot_counter"
grep -q 'Kernel panic' "$L" && miss "unarmed: a panic"

if [ "$pass" = 1 ]; then
    echo ">> PASS: real GRUB counted down, fell back to schema_fallback_id after a dead boot, and never fell back unarmed"
else
    for f in armed unarmed; do [ -f "$WORK/$f.txt" ] && { echo "---- $f serial (CI/GRUB/panic lines)"; grep -aE 'CI-|panic|error|blscfg' "$WORK/$f.txt" | head -40; }; done
    exit 1
fi
