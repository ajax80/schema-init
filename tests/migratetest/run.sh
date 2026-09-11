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
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ART="$HERE/artifacts"
REPO="$(cd "$HERE/../.." && pwd)"
ISO="$ART/Fedora-Everything-netinst-44.iso"
ISO_LABEL="Fedora-E-dvd-x86_64-44"
STOCK="$ART/stock-clean.qcow2"     # pristine post-install snapshot
WORK="$ART/work.qcow2"             # mutable copy the migration runs against
SSHKEY="$ART/id_migrate"
SSHPORT=2222
MON="$ART/qmp.sock"
KEY_SSH=(ssh -i "$SSHKEY" -p "$SSHPORT" -o StrictHostKeyChecking=no \
         -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR)

log() { printf '\n\033[1;36m>> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

RAM=${RAM:-3072}; CPUS=${CPUS:-2}
have_kvm() { [ -w /dev/kvm ] && echo "-accel kvm -cpu host" || echo "-accel tcg"; }

# ---- shared qemu invocation for an installed disk (legacy BIOS) --------------
boot_disk() {  # $1=image  $2=extra qemu args...  (foreground, serial console)
    local img="$1"; shift
    qemu-system-x86_64 \
        -name schema-migrate-vm -machine pc -m "$RAM" -smp "$CPUS" $(have_kvm) \
        -drive file="$img",if=virtio,format=qcow2,cache=writeback \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:${SSHPORT}-:22 \
        -device virtio-net-pci,netdev=n0,csum=off,gso=off,guest_csum=off,guest_tso4=off,guest_tso6=off,host_tso4=off,host_tso6=off \
        -nographic -serial mon:stdio "$@"
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
    local pub; pub="$(cat "$ART/id_migrate.pub")"
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

fresh_work() {
    [ -f "$STOCK" ] || die "no stock-clean.qcow2 — run './run.sh install' first"
    log "creating work overlay on pristine stock (copy-on-write)"
    rm -f "$WORK"
    qemu-img create -f qcow2 -F qcow2 -b "$STOCK" "$WORK" >/dev/null || die "overlay create"
}

VMPID="$ART/vm.pid"
SERLOG="$ART/boot-serial.log"

boot_bg() {  # $1=image — detached qemu, serial appended to log, survives guest reboot
    local img="$1"
    : > "$SERLOG"
    qemu-system-x86_64 \
        -name schema-migrate-vm -machine pc -m "$RAM" -smp "$CPUS" $(have_kvm) \
        -drive file="$img",if=virtio,format=qcow2,cache=writeback \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:${SSHPORT}-:22 \
        -device virtio-net-pci,netdev=n0,csum=off,gso=off,guest_csum=off,guest_tso4=off,guest_tso6=off,host_tso4=off,host_tso6=off -display none \
        -chardev file,id=ser0,path="$SERLOG",append=on -serial chardev:ser0 \
        -monitor unix:"$MON",server,nowait -pidfile "$VMPID" &
    disown
    sleep 2
}
vm_kill() { [ -f "$VMPID" ] && kill "$(cat "$VMPID")" 2>/dev/null; rm -f "$VMPID" "$MON"; }
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
    wait_ssh 300 || die "stock VM never reached ssh"
    local exe; exe=$(pid1); log "stock PID1 = $exe"
    [[ "$exe" == *systemd ]] || die "expected systemd on stock, got '$exe'"

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
          2>&1 | tee "$ART/deploy.log"

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
    local stg; stg=$(rootc "python3 /home/tester/schema-init/distros/fedora-installer/migrate/schema-migrate.py --stage")
    echo "  post-deploy stage: $stg"
    [ "$stg" = "R1_PENDING" ] || die "expected R1_PENDING after deploy, got '$stg'"

    local title; title=$(rootc "grep '^title ' /boot/loader/entries/schema-init.conf | sed 's/^title //'")
    [ -n "$title" ] && echo "  schema BLS entry: '$title'" || die "no schema-init.conf BLS entry written"
    log "menu-visibility check: is the fallback entry selectable by a human at the console?"
    rootc "grep -E 'GRUB_TIMEOUT=' /etc/default/grub || echo '(GRUB_TIMEOUT unset)'" | sed 's/^/      /'

    log "setting one-time boot to the schema entry and rebooting"
    rootc "grub2-reboot \"$title\" && systemctl reboot" || true
    sleep 5
    wait_ssh 300 || { panic_in_serial && die "schema boot PANICKED (see $SERLOG)"; die "schema VM never came back on ssh"; }
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
    for _ in $(seq 1 15); do
        stg=$(rootc "python3 /home/tester/schema-init/distros/fedora-installer/migrate/schema-migrate.py --stage" 2>/dev/null)
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
    for _ in $(seq 1 15); do
        st=$(rootc "cat /run/schema-init/doctor-status 2>/dev/null")
        [ -n "$st" ] && break
        sleep 3
    done
    [ -z "$st" ] && st=$(rootc "schema-doctor --heal --wait 30 2>&1; schema-doctor --status 2>&1")
    echo "$st" | sed 's/^/    /'
    log "VERIFY OK — schema-init is PID1 on the migrated box"
}

do_uninstall() {
    trap vm_kill EXIT
    [ -f "$VMPID" ] || { boot_bg "$WORK"; wait_ssh 300 || die "VM not up for uninstall"; }
    log "running schema-migrate --uninstall"
    rootc "MIGRATE_REPO=/home/tester/schema-init python3 \
          /home/tester/schema-init/distros/fedora-installer/migrate/schema-migrate.py --uninstall" \
          2>&1 | tee "$ART/uninstall.log"
    log "rebooting into stock (default entry)"
    rootc "systemctl reboot" || true
    sleep 5
    wait_ssh 300 || die "VM never came back after uninstall reboot"
    local exe; exe=$(pid1); echo "  PID1 = $exe"
    [[ "$exe" == *systemd ]] || die "expected systemd back after uninstall, got '$exe'"
    log "UNINSTALL OK — box is back on stock systemd"
}

case "${1:-}" in
    install)   do_install ;;
    fresh)     fresh_work ;;
    boot)      boot_disk "${2:-$WORK}" ;;
    boot-bg)   boot_bg "${2:-$WORK}" ; echo "backgrounded; serial: $SERLOG" ;;
    migrate)   do_migrate ;;
    verify)    do_verify ;;
    uninstall) do_uninstall ;;
    kill)      vm_kill ;;
    ssh)       shift; sshc "$@" ;;
    all)       do_install; do_migrate; do_uninstall ;;
    *) echo "usage: $0 {install|migrate|verify|uninstall|all|boot|boot-bg|ssh|kill|fresh}"; exit 2 ;;
esac
