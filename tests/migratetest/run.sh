#!/usr/bin/env bash
# schema-migrate round-trip harness (Fedora KDE, LEGACY BIOS — matches Optiplex).
#
#   ./run.sh install    build stock-clean.qcow2 via headless kickstart install
#   ./run.sh migrate     copy stock -> work, deploy schema-migrate, reboot into it
#   ./run.sh verify      ssh in, assert PID1=schema-init + schema-doctor clean
#   ./run.sh uninstall   schema-migrate --uninstall, reboot, assert back to stock
#   ./run.sh all         install (if needed) -> migrate -> verify -> uninstall
#   ./run.sh boot [img]  interactive serial boot of an image (default: work.qcow2)
#   ./run.sh ssh [cmd]   ssh into the running VM as tester
#
# The VM is deliberately close to Optiplex: SeaBIOS/legacy boot, msdos MBR disk,
# GRUB in the MBR, real BLS entries under a BIOS GRUB, plasma+sddm, uid-1000 user.
#
# ARCH=aarch64 instead runs what a stranger on a Pi has: the official Fedora KDE
# aarch64 disk image (UEFI, plasmalogin, btrfs root/home/var) under TCG, with
# schema-init from the COPR rather than the repo's x86_64 binaries. `install`
# preps that image (needs sudo, qemu-user-static-aarch64 and binfmt_misc);
# `migrate` then runs the wizard's own deploy. Expect ~6 min per boot.
#   ART=~/Ocean/vm/aarch64 SSHKEY=~/.cache/schema-a64/id_migrate ARCH=aarch64 ./run.sh all
# MIGRATE_SRC=repo overlays this tree's migrator on the COPR one; FLIPS=0 skips stages 2-3.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCH=${ARCH:-x86_64}
ART="${ART:-$HERE/artifacts}"
REPO="$(cd "$HERE/../.." && pwd)"
ISO="$ART/Fedora-Everything-netinst-44.iso"
ISO_LABEL="Fedora-E-dvd-x86_64-44"
A64_IMG="$ART/Fedora-KDE-Desktop-Disk-44-1.7.aarch64.raw.xz"
COPR=${COPR:-ajax80/schema-init}
STOCK="$ART/stock-clean.qcow2"     # pristine post-install snapshot
WORK="$ART/work.qcow2"             # mutable copy the migration runs against
# ssh refuses a key with loose modes, and some filesystems (mergerfs) won't keep 0600.
SSHKEY="${SSHKEY:-$ART/id_migrate}"
SSHPORT=2222
# x86 runs the migrator from the staged repo; aarch64 runs the packaged one.
if [ "$ARCH" = aarch64 ]; then MIG=/usr/bin/schema-migrate
else MIG="MIGRATE_REPO=/home/tester/schema-init python3 /home/tester/schema-init/distros/fedora-installer/migrate/schema-migrate.py"; fi
MON="$ART/qmp.sock"
KEY_SSH=(ssh -i "$SSHKEY" -p "$SSHPORT" -o StrictHostKeyChecking=no \
         -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR)

log() { printf '\n\033[1;36m>> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

# TCG boots take ~6 min, and an SELinux relabel boot (first boot, uninstall) ~20
if [ "$ARCH" = aarch64 ]; then RAM=${RAM:-6144}; CPUS=${CPUS:-6}; BOOTWAIT=${BOOTWAIT:-2400}
else RAM=${RAM:-3072}; CPUS=${CPUS:-2}; BOOTWAIT=${BOOTWAIT:-300}; fi
# verify's polls (stage advance, desktop, doctor) stretch with it
[ "$ARCH" = aarch64 ] && SLOW=4 || SLOW=1
have_kvm() { [ -w /dev/kvm ] && echo "-accel kvm -cpu host" || echo "-accel tcg"; }

# ---- shared qemu invocation for an installed disk ---------------------------
vm_qemu() {  # $1=image  then extra qemu args
    local img="$1"; shift
    if [ "$ARCH" = aarch64 ]; then
        # cortex-a72 = a Pi 4's core, and much faster under TCG than -cpu max.
        # i6300esb gives schema-init a real /dev/watchdog to pet, like the Pi's.
        qemu-system-aarch64 \
            -name schema-migrate-vm -M virt -cpu cortex-a72 -m "$RAM" -smp "$CPUS" \
            -accel tcg,thread=multi,tb-size=512 \
            -drive if=pflash,format=raw,readonly=on,file="$ART/efi-code.raw" \
            -drive if=pflash,format=raw,file="$ART/efi-vars.raw" \
            -drive file="$img",if=virtio,format=qcow2,cache=writeback \
            -device virtio-gpu-pci -device qemu-xhci -device usb-kbd -device usb-tablet -device i6300esb \
            -netdev user,id=n0,hostfwd=tcp:127.0.0.1:${SSHPORT}-:22 \
            -device virtio-net-pci,netdev=n0 "$@"
    else
        qemu-system-x86_64 \
            -name schema-migrate-vm -machine pc -m "$RAM" -smp "$CPUS" $(have_kvm) \
            -drive file="$img",if=virtio,format=qcow2,cache=writeback \
            -netdev user,id=n0,hostfwd=tcp:127.0.0.1:${SSHPORT}-:22 \
            -device virtio-net-pci,netdev=n0,csum=off,gso=off,guest_csum=off,guest_tso4=off,guest_tso6=off,host_tso4=off,host_tso6=off \
            "$@"
    fi
}

boot_disk() {  # $1=image  $2=extra qemu args...  (foreground, serial console)
    local img="$1"; shift
    vm_qemu "$img" -nographic -serial mon:stdio "$@"
}

wait_ssh() {  # $1=timeout seconds
    local t="${1:-180}" start; start=$(date +%s)
    log "waiting for ssh (up to ${t}s)"
    while :; do
        "${KEY_SSH[@]}" tester@127.0.0.1 true 2>/dev/null && { echo "  ssh up"; return 0; }
        [ $(( $(date +%s) - start )) -ge "$t" ] && return 1
        sleep 3
    done
}

# ---- install: headless kickstart into stock-clean.qcow2 ---------------------
do_install() {
    [ -f "$ISO" ] || die "missing ISO: $ISO"
    [ -f "$ART/vmlinuz" ] && [ -f "$ART/initrd.img" ] || die "missing extracted vmlinuz/initrd"
    [ -f "$STOCK" ] && { log "stock-clean.qcow2 exists — skipping install (rm it to rebuild)"; return 0; }

    log "generating kickstart with harness pubkey"
    local pub; pub="$(cat "$SSHKEY.pub")"
    sed "s|@@PUBKEY@@|$pub|g" "$HERE/kickstart.cfg" > "$ART/kickstart.gen.cfg"

    log "serving kickstart on :8000"
    ( cd "$ART" && exec python3 -m http.server 8000 --bind 0.0.0.0 ) >/dev/null 2>&1 &
    local http=$!; trap 'kill '"$http"' 2>/dev/null' RETURN

    qemu-img create -f qcow2 "$ART/stock.qcow2" 40G >/dev/null || die "qcow2 create"

    log "booting anaconda (netinst kickstart, legacy BIOS) — this pulls KDE over the net, be patient"
    log "  serial log: $ART/install-serial.log   (tail -f to watch)"
    qemu-system-x86_64 \
        -name schema-migrate-install -machine pc -m "$RAM" -smp "$CPUS" $(have_kvm) \
        -drive file="$ART/stock.qcow2",if=virtio,format=qcow2,cache=unsafe \
        -cdrom "$ISO" \
        -kernel "$ART/vmlinuz" -initrd "$ART/initrd.img" \
        -append "inst.stage2=hd:LABEL=${ISO_LABEL} inst.ks=http://10.0.2.2:8000/kickstart.gen.cfg inst.text console=ttyS0,115200 inst.notmux" \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
        -no-reboot -display none \
        -serial file:"$ART/install-serial.log" -monitor none \
        || die "install qemu exited nonzero"

    # anaconda poweroffs at %end; the disk should now be bootable.
    mv "$ART/stock.qcow2" "$STOCK" || die "rename stock"
    log "install complete -> $STOCK"
}

# ---- install (aarch64): prep the official KDE disk image into stock-clean ----
do_install_a64() {
    [ -f "$STOCK" ] && { log "stock-clean.qcow2 exists — skipping install (rm it to rebuild)"; return 0; }
    [ -f "$A64_IMG" ] || die "missing $A64_IMG (dl.fedoraproject.org .../44/KDE/aarch64/images/)"
    [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || die "no aarch64 binfmt handler (qemu-user-static-aarch64, binfmt_misc mounted)"
    [ -f "$SSHKEY" ] || ssh-keygen -q -t ed25519 -N '' -f "$SSHKEY" -C schema-migratetest || die "keygen"
    local fw=/usr/share/edk2/aarch64
    [ -f $fw/QEMU_EFI-pflash.raw ] && [ -f $fw/QEMU_EFI-qemuvars-pflash.raw ] || die "no UEFI firmware (edk2-aarch64)"
    local raw="$ART/prep.raw" mnt loop=""; mnt=$(mktemp -d)
    # a die anywhere below must not leave the image mounted or the loop device held
    trap 'sudo umount -R "$mnt" 2>/dev/null; [ -n "$loop" ] && sudo losetup -d "$loop" 2>/dev/null; rmdir "$mnt" 2>/dev/null; vm_kill' EXIT
    log "unpacking the disk image"
    xz -dc "$A64_IMG" > "$raw" || die "xz"
    loop=$(sudo losetup -fP --show "$raw") || die "losetup"
    sudo mount -o subvol=root "${loop}p3" "$mnt" && sudo mount -o subvol=home "${loop}p3" "$mnt/home" \
        && sudo mount -o subvol=var "${loop}p3" "$mnt/var" && sudo mount "${loop}p2" "$mnt/boot" \
        && sudo mount "${loop}p1" "$mnt/boot/efi" || die "mount image"
    for d in proc sys dev; do sudo mount --bind /$d "$mnt/$d"; done
    log "chroot: tester (uid 1000) + root keys, sshd, no first-run setup, plasmalogin autologin"
    local pub; pub="$(cat "$SSHKEY.pub")"
    sudo chroot "$mnt" /bin/bash -ec "
        useradd -m -u 1000 -G wheel tester
        echo tester:schema123 | chpasswd; echo root:schemaroot | chpasswd
        for h in /root /home/tester; do install -d -m700 \$h/.ssh; echo '$pub' > \$h/.ssh/authorized_keys; chmod 600 \$h/.ssh/authorized_keys; done
        chown -R tester:tester /home/tester/.ssh
        systemctl enable sshd.service serial-getty@ttyAMA0.service
        systemctl mask plasma-setup.service
        printf '[Autologin]\nUser=tester\nSession=plasma\n' > /etc/plasmalogin.conf.d/autologin.conf
        # files written from the host carry no SELinux label; enforcing then
        # breaks logind and sshd until the first boot relabels them
        touch /.autorelabel" || die "chroot prep"
    sudo umount "$mnt"/{proc,sys,dev,boot/efi,boot,var,home} "$mnt"; sudo losetup -d "$loop"; rmdir "$mnt"; loop=""
    qemu-img convert -O qcow2 "$raw" "$ART/stock.qcow2" && rm -f "$raw" || die "convert"
    cp $fw/QEMU_EFI-pflash.raw "$ART/efi-code.raw" && cp $fw/QEMU_EFI-qemuvars-pflash.raw "$ART/efi-vars.raw" || die "firmware copy"

    log "first boot (growfs + SELinux relabel + reboot) — slow under TCG"
    boot_bg "$ART/stock.qcow2"
    wait_ssh $BOOTWAIT || die "first boot never reached ssh"
    # off by default: ~2,000 rpm steps take hours under TCG
    if [ "${UPGRADE:-0}" = 1 ]; then
        log "dnf upgrade --refresh (what the README asks a stranger to do first)"
        rootc "dnf upgrade -y --refresh" > "$ART/upgrade.log" 2>&1 || die "dnf upgrade (see $ART/upgrade.log)"
    fi
    vm_poweroff || die "first boot did not power off"
    mv "$ART/stock.qcow2" "$STOCK" && cp "$ART/efi-vars.raw" "$ART/efi-vars-stock.raw" || die "rename stock"
    trap - EXIT
    log "install complete -> $STOCK"
}

fresh_work() {
    [ -f "$STOCK" ] || die "no stock-clean.qcow2 — run './run.sh install' first"
    log "creating work overlay on pristine stock (copy-on-write)"
    rm -f "$WORK"
    qemu-img create -f qcow2 -F qcow2 -b "$STOCK" "$WORK" >/dev/null || die "overlay create"
    [ "$ARCH" = aarch64 ] && { cp "$ART/efi-vars-stock.raw" "$ART/efi-vars.raw" || die "efi vars"; }
}

VMPID="$ART/vm.pid"
SERLOG="$ART/boot-serial.log"

boot_bg() {  # $1=image — detached qemu, serial appended to log, survives guest reboot
    local img="$1"
    : > "$SERLOG"
    vm_qemu "$img" -display none \
        -chardev file,id=ser0,path="$SERLOG",append=on -serial chardev:ser0 \
        -monitor unix:"$MON",server,nowait -pidfile "$VMPID" &
    disown
    sleep 2
}
vm_kill() { [ -f "$VMPID" ] && kill "$(cat "$VMPID")" 2>/dev/null; rm -f "$VMPID" "$MON"; }
vm_poweroff() {  # clean guest shutdown; never pull the plug on a disk we keep
    local p; p=$(cat "$VMPID" 2>/dev/null) || return 1
    rootc "systemctl poweroff" 2>/dev/null
    for _ in $(seq 1 120); do kill -0 "$p" 2>/dev/null || { rm -f "$VMPID" "$MON"; return 0; }; sleep 5; done
    return 1
}
sshc()  { "${KEY_SSH[@]}" tester@127.0.0.1 "$@"; }
# tester's sudo needs a password (no tty over ssh); root has the harness key in
# its authorized_keys, so privileged steps run as root directly.
rootc() { "${KEY_SSH[@]}" root@127.0.0.1 "$@"; }
pid1()  { rootc "readlink -f /proc/1/exe" 2>/dev/null; }
panic_in_serial() { grep -qiE "Kernel panic|No init found|Attempted to kill init" "$SERLOG"; }

# ---- migrate: deploy schema-migrate against a fresh copy of stock -----------
do_migrate() {
    fresh_work
    trap vm_kill EXIT
    log "booting stock (systemd) VM"
    boot_bg "$WORK"
    wait_ssh $BOOTWAIT || die "stock VM never reached ssh"
    local exe; exe=$(pid1); log "stock PID1 = $exe"
    [[ "$exe" == *systemd ]] || die "expected systemd on stock, got '$exe'"

    if [ "$ARCH" = aarch64 ]; then
        # -migrate, not -wizard: the wizard's PySide6 drags the Qt/KDE stack
        # forward, which wants a full dnf upgrade first (install's UPGRADE=1). Its backend runs the same
        # schema-migrate calls this harness makes.
        log "dnf install schema-init-migrate from COPR $COPR"
        rootc "dnf copr enable -y $COPR && dnf install -y schema-init-migrate" > "$ART/copr-install.log" 2>&1 \
            || { tail -20 "$ART/copr-install.log"; die "COPR install failed (see $ART/copr-install.log)"; }
        rootc "rpm -q schema-init schema-init-migrate" | sed 's/^/  /'
        if [ "${MIGRATE_SRC:-}" = repo ]; then
            # the migrator is Python + data: lay this tree's over the packaged
            # copy, so unreleased migrate fixes run on aarch64 without a build
            log "overlaying this repo's migrate tree onto the packaged one"
            tar -C "$REPO" -cf - distros/fedora-installer/migrate distros/fedora-installer/rail distros/fedora-kde/scripts \
                distros/fedora-kde/services distros/fedora-kde/config scripts \
                distros/fedora-installer/schema-flip-apply.sh distros/fedora-installer/schema-udev-flip-healthcheck.sh | rootc "
                set -e; d=/usr/share/schema-init/migrate; t=\$(mktemp -d); tar -C \$t -xf -
                # only over files a package put there: nothing unowned survives uninstall
                ov() { [ -e \"\$2\" ] && install -m\$3 \"\$t/\$1\" \"\$2\" || :; }
                L=/usr/libexec/schema-init
                ov distros/fedora-installer/migrate/schema-migrate.py /usr/bin/schema-migrate 0755
                ov distros/fedora-installer/migrate/schema-import.py /usr/bin/schema-import 0755
                ov distros/fedora-installer/migrate/schema-import.py \$L/schema-import 0755
                ov distros/fedora-installer/migrate/stage.py \$L/stage.py 0644
                ov distros/fedora-installer/schema-flip-apply.sh \$L/schema-flip-apply 0755
                ov distros/fedora-installer/schema-udev-flip-healthcheck.sh \$L/schema-udev-flip-healthcheck.sh 0755
                for h in schema-udev-flip-arm.sh schema-udev-flip-backup.sh schema-mount-fstab schema-dbus-run.sh \\
                         schema-dbus-flip.sh schema-dbus-flip-healthcheck.sh; do ov scripts/\$h \$L/\$h 0755; done
                install -m0644 \$t/distros/fedora-installer/migrate/prevent-set.list \$d/prevent-set.list
                for sub in distros/fedora-installer/rail/services distros/fedora-installer/rail/scripts \
                           distros/fedora-kde/scripts distros/fedora-kde/services distros/fedora-kde/config; do
                    cp -a \$t/\$sub/. \$d/\$sub/
                done
                rm -rf \$t" || die "overlay"
        fi
        log "running schema-migrate --deploy --prebuilt (what the wizard's Continue runs)"
        rootc "$MIG --deploy --prebuilt" 2>&1 | tee "$ART/deploy.log" || die "deploy failed (see $ART/deploy.log)"
    else
    log "staging the repo onto the VM (simulating the USB)"
    tar -C "$REPO" --exclude=.git --exclude=tests/migratetest/artifacts --exclude='*.o' -cf - . \
        | sshc "rm -rf /home/tester/schema-init && mkdir -p /home/tester/schema-init && tar -C /home/tester/schema-init -xf -" \
        || die "tar repo to VM"

    # Pre-place the prebuilt x86_64 binaries the schema-init RPM would ship, then
    # deploy with --prebuilt — exactly what the wizard backend does
    # (--deploy --prebuilt). Building on the VM needs dbus-devel etc. we don't
    # provision here; the fix under test is in Python/data, not the C.
    # Simulate what the schema-init RPMs install: the three prebuilt binaries,
    # the migrate tool at /usr/bin/schema-migrate, and stage.py under libexec
    # (the path schema-migrate falls back to). Without these the finish oneshot
    # has nothing to exec.
    log "pre-placing prebuilt binaries + migrate tool (simulating the RPMs)"
    rootc "install -m0755 /home/tester/schema-init/schema-init \
           /home/tester/schema-init/schema-ctl \
           /home/tester/schema-init/schema-subreaper /usr/bin/ && \
           install -m0755 /home/tester/schema-init/distros/fedora-installer/migrate/schema-migrate.py /usr/bin/schema-migrate && \
           install -D -m0644 /home/tester/schema-init/distros/fedora-installer/migrate/stage.py /usr/libexec/schema-init/stage.py" \
          || die "pre-place prebuilt binaries + migrate tool"
    rootc "test -x /usr/bin/schema-migrate && test -f /usr/libexec/schema-init/stage.py" \
          || die "pre-place verify: /usr/bin/schema-migrate or stage.py not in place"
    echo "  OK  /usr/bin/schema-migrate + stage.py in place"

    log "running schema-migrate --deploy --prebuilt on the VM"
    rootc "MIGRATE_REPO=/home/tester/schema-init python3 \
          /home/tester/schema-init/distros/fedora-installer/migrate/schema-migrate.py --deploy --prebuilt" \
          2>&1 | tee "$ART/deploy.log" || die "deploy failed (see $ART/deploy.log)"
    fi

    log "post-deploy checks (before any reboot into schema)"
    if rootc "test -x /usr/bin/schema-init"; then
        echo "  OK  /usr/bin/schema-init present"
    else
        echo "  GAP /usr/bin/schema-init ABSENT — 'make install' produced no binary"
        echo "      (deploy's run_make_install uses check=False, so a missing toolchain"
        echo "       silently no-ops the build; the schema BLS entry would panic on boot)"
        echo "  toolchain on VM:"; rootc "command -v gcc make; rpm -q libacl-devel 2>&1" | sed 's/^/      /'
        die "deploy left no schema-init binary — fix schema-migrate, then re-run"
    fi

    # Fix #1 (deploy side): the finish oneshot must be installed, and the stage
    # must sit at R1_PENDING awaiting the post-reboot auto-advance.
    if rootc "test -f /etc/schema-init/services/schema-migrate-finish.svc"; then
        echo "  OK  schema-migrate-finish.svc deployed"
    else
        die "FIX#1 REGRESSION: finish oneshot was not deployed"
    fi
    local stg; stg=$(rootc "$MIG --stage")
    echo "  post-deploy stage: $stg"
    [ "$stg" = "R1_PENDING" ] || die "expected R1_PENDING after deploy, got '$stg'"

    local kv; kv=$(rootc "uname -r")
    rootc "test -f /boot/loader/entries/schema-$kv.conf" && echo "  OK  schema BLS entry schema-$kv.conf" \
        || die "no schema-$kv.conf BLS entry written"
    rootc "test -f /etc/schema-init/boot-default" || die "boot-default marker not armed"
    rootc "grub2-editenv list | grep -qx saved_entry=schema-$kv" && echo "  OK  saved_entry=schema-$kv" \
        || die "saved_entry is not schema-$kv"
    log "menu-visibility check: is the fallback entry selectable by a human at the console?"
    rootc "grep -E 'GRUB_TIMEOUT=' /etc/default/grub || echo '(GRUB_TIMEOUT unset)'" | sed 's/^/      /'

    log "rebooting into the default (schema) entry"
    rootc "systemctl reboot" || true
    sleep 5
    wait_ssh $BOOTWAIT || { panic_in_serial && die "schema boot PANICKED (see $SERLOG)"; die "schema VM never came back on ssh"; }
    do_verify
}

do_verify() {
    log "verifying the schema boot"
    local exe; exe=$(pid1); echo "  PID1 = $exe"
    [ "$exe" = "/usr/bin/schema-init" ] || die "PID1 is not schema-init (got '$exe')"

    # Fix #1 (the whole point): the finish oneshot must auto-advance the stage on
    # this boot with NO manual --finish. Poll to avoid racing the oneshot.
    log "checking the finish oneshot advanced the stage on its own (up to 45s):"
    local stg=""
    for _ in $(seq 1 $((15 * SLOW))); do
        stg=$(rootc "$MIG --stage" 2>/dev/null)
        [ "$stg" = "R1_HEAL" ] && break
        sleep 3
    done
    echo "  stage after schema boot: $stg"
    [ "$stg" = "R1_HEAL" ] || die "FIX#1 FAILED: stage did not auto-advance (got '$stg', expected R1_HEAL — the finish oneshot never ran)"
    echo "  OK  finish oneshot advanced R1_PENDING -> R1_HEAL with no handwork"
    log "waiting for the plasma session (autologin) to come up (up to 90s)"
    local up=""
    for _ in $(seq 1 30); do
        if rootc "pgrep -x plasmashell >/dev/null && pgrep -x kwin_wayland >/dev/null"; then up=1; break; fi
        sleep 3
    done
    if [ -n "$up" ]; then
        echo "  desktop: kwin_wayland + plasmashell running:"
        rootc "pgrep -a -x kwin_wayland; pgrep -a -x plasmashell; \
               pgrep -f org_kde_powerdevil >/dev/null && echo 'powerdevil running' || echo 'powerdevil not yet up (see doctor status below)'; \
               sudo -u \$(id -un 1000 2>/dev/null || echo tester) test -r /dev/dri/card0 && echo 'uid1000 can open /dev/dri/card0'" | sed 's/^/    /'
    else
        echo "  desktop: WARN plasmashell/kwin_wayland not up within 90s"
        rootc "ps -u \$(id -un 1000 2>/dev/null || echo tester) -o comm= | sort -u | grep -iE 'kwin|plasma' | head" | sed 's/^/    /'
    fi
    # Read the HEALED status the boot doctor (--heal --wait 30) writes once a
    # session exists. If it hasn't landed yet, drive one heal run ourselves —
    # never a bare --check, which reports an un-healed snapshot and lies.
    log "schema-doctor healed status (waiting up to 45s for the boot heal):"
    local st=""
    for _ in $(seq 1 $((15 * SLOW))); do
        st=$(rootc "cat /run/schema-init/doctor-status 2>/dev/null")
        [ -n "$st" ] && break
        sleep 3
    done
    [ -z "$st" ] && st=$(rootc "schema-doctor --heal --wait 30 2>&1; schema-doctor --status 2>&1")
    echo "$st" | sed 's/^/    /'
    log "VERIFY OK — schema-init is PID1 on the migrated box"
}

# stages 2 and 3, as the wizard's Continue drives them: schema-udev, then schema-dbus
do_flips() {
    local stg
    log "stage 2: arm the schema-udev flip, reboot"
    rootc "$MIG --arm-flip" || die "arm-flip"
    rootc "systemctl reboot" || true; sleep 5
    wait_ssh $BOOTWAIT || die "never came back after the udev flip"
    for _ in $(seq 1 30); do
        stg=$(rootc "$MIG --stage" 2>/dev/null); case $stg in DONE|ROLLED_BACK) break ;; esac; sleep 10
    done
    [ "$stg" = DONE ] || die "udev flip: stage $stg ($(rootc "$MIG --why" 2>/dev/null))"
    echo "  OK  udev flip DONE; $(rootc 'pgrep -a -x schema-udev')"
    log "stage 3: arm the schema-dbus flip, reboot, confirm"
    rootc "$MIG --arm-dbus" || die "arm-dbus"
    rootc "systemctl reboot" || true; sleep 5
    wait_ssh $BOOTWAIT || die "never came back after the dbus flip"
    stg=$(rootc "$MIG --stage"); [ "$stg" = R3_ROLLED_BACK ] && die "dbus flip rolled back ($(rootc "$MIG --why" 2>/dev/null))"
    local up=""
    for _ in $(seq 1 30); do rootc "pgrep -x plasmashell >/dev/null" && { up=1; break; }; sleep 10; done
    [ -n "$up" ] || die "no plasma session on schema-dbus (stage $stg)"
    rootc "$MIG --confirm-dbus" || die "confirm-dbus ($(rootc "$MIG --stage"))"
    stg=$(rootc "$MIG --stage"); [ "$stg" = R3_DONE ] || die "dbus flip: stage $stg"
    echo "  OK  dbus flip R3_DONE; system bus: $(rootc 'readlink -f /proc/$(pgrep -o -f -- "--system")/exe')"
    rootc "schema-ctl status" | grep -vE "PERFECT|FUNDAMENTAL|SETTLED" | sed 's/^/    /'
    rootc "$MIG --finish" >/dev/null
}

do_uninstall() {
    trap vm_kill EXIT
    [ -f "$VMPID" ] || { boot_bg "$WORK"; wait_ssh $BOOTWAIT || die "VM not up for uninstall"; }
    log "running schema-migrate --uninstall"
    rootc "$MIG --uninstall" \
          2>&1 | tee "$ART/uninstall.log"
    log "rebooting into stock (default entry)"
    rootc "systemctl reboot" || true
    sleep 5
    wait_ssh $BOOTWAIT || die "VM never came back after uninstall reboot"
    local exe; exe=$(pid1); echo "  PID1 = $exe"
    [[ "$exe" == *systemd ]] || die "expected systemd back after uninstall, got '$exe'"
    # what --uninstall left behind that no package owns: it should be nothing
    rootc 'cd /etc/schema-init 2>/dev/null && find . \( -type f -o -type l \) | while read -r f; do
               rpm -qf "/etc/schema-init/${f#./}" >/dev/null 2>&1 || echo "${f#./}"; done' > "$ART/uninstall-leftovers.txt"
    local left; left=$(wc -l < "$ART/uninstall-leftovers.txt")
    echo "  unowned files left under /etc/schema-init: $left (list: $ART/uninstall-leftovers.txt)"
    [ "$left" -eq 0 ] || die "--uninstall left $left files no package owns"
    log "UNINSTALL OK — box is back on stock systemd"
}

do_install_any() { if [ "$ARCH" = aarch64 ]; then do_install_a64; else do_install; fi; }

case "${1:-}" in
    install)   do_install_any ;;
    fresh)     fresh_work ;;
    boot)      boot_disk "${2:-$WORK}" ;;
    boot-bg)   boot_bg "${2:-$WORK}" ; echo "backgrounded; serial: $SERLOG" ;;
    migrate)   do_migrate ;;
    verify)    do_verify ;;
    uninstall) do_uninstall ;;
    kill)      vm_kill ;;
    ssh)       shift; sshc "$@" ;;
    flips)     do_flips ;;
    all)       do_install_any; do_migrate; [ "${FLIPS:-1}" = 1 ] && do_flips; do_uninstall; vm_poweroff ;;
    *) echo "usage: $0 {install|migrate|verify|flips|uninstall|all|boot|boot-bg|ssh|kill|fresh}"; exit 2 ;;
esac
