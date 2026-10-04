#!/bin/bash
# Build a schema installer ISO = Fedora 44 Everything/netinst + kickstart + payload.
# Uses mkksiso (from lorax); the user still gets the real Anaconda GUI, schema.ks
# only adds a silent %post and a first-boot wizard.
#
#   ./build-iso.sh /path/to/Fedora-Everything-Netinst-x86_64-44-1.7.iso [out.iso]
#
# NOTE: some Fedora ISOs carry duplicate case-variant EFI files (bootx64.efi +
# BOOTX64.EFI) that make mkksiso's mkefiboot fail with "File exists". If that
# bites this base too, the fallback is a manual `xorriso -boot_image any replay`
# repack that preserves the original boot images and only patches grub.cfg to
# append inst.ks — no EFI-image rebuild. (netinst boots straight to Anaconda, so
# a grub.cfg inst.ks= patch is sufficient; no mkefiboot needed.)
#
# Prereqs on the build host:  dnf install lorax   (provides mkksiso)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BASE_ISO="${1:?usage: build-iso.sh <fedora-iso> [out.iso]}"
OUT="${2:-$HOME/schema-fedora44-installer.iso}"
PAYLOAD="$(mktemp -d)/schema"
trap 'rm -rf "$(dirname "$PAYLOAD")"' EXIT

command -v mkksiso >/dev/null || { echo "need mkksiso: sudo dnf install lorax" >&2; exit 1; }

echo "=== building schema-init binaries (current branch) ==="
make -C "$REPO" schema-init schema-ctl schema-journal-sink schema-subreaper schema-board schema-udev schema-dbus >/dev/null
make -C "$REPO" verify-rules-live >/dev/null 2>&1 || true   # if it has a make target

echo "=== staging payload the %post copies from (ISO:/schema) ==="
mkdir -p "$PAYLOAD/bin" "$PAYLOAD/scripts" "$PAYLOAD/services"
for b in schema-init schema-ctl schema-journal-sink schema-subreaper schema-board schema-udev schema-dbus verify-rules-live; do
    install -m0755 "$REPO/$b" "$PAYLOAD/bin/$b"
done
install -m0755 "$REPO/scripts/schema-udev-flip-arm.sh"    "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-udev-flip-backup.sh" "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/gen-services.sh"            "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/gen-mounts.sh"              "$PAYLOAD/scripts/"
install -m0755 "$HERE/firstboot-flip-wizard.sh"           "$PAYLOAD/scripts/"
install -m0755 "$HERE/schema-flip-apply.sh"               "$PAYLOAD/scripts/"
install -m0755 "$HERE/schema-udev-flip-healthcheck.sh"    "$PAYLOAD/scripts/"
install -m0755 "$HERE/rail/scripts/schema-sysprep.sh"          "$PAYLOAD/scripts/"
install -m0755 "$HERE/rail/scripts/schema-sshd-start.sh"       "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-sysctl-apply"              "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-mount-fstab"               "$PAYLOAD/scripts/"
install -m0755 "$HERE/rail/scripts/schema-zram-start.sh"       "$PAYLOAD/scripts/"
install -m0755 "$HERE/../fedora-kde/scripts/schema-plasma-autologin.sh" "$PAYLOAD/scripts/"
# Desktop-session pipeline, generalized from distros/fedora-kde: the native
# login1 stub + session bookkeeping + the Plasma launch chain. Autologin drives
# these to bring up a full KDE Wayland desktop under schema-init as PID 1.
install -m0755 "$REPO/scripts/schema-logind.py"               "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-doctor.py"               "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-session-register"        "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-session-unregister"      "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-dbus-session-run.sh"      "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-dbus-run.sh"              "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-dbus-flip.sh"             "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-dbus-flip-healthcheck.sh" "$PAYLOAD/scripts/"
install -m0755 "$REPO/tools/dbus-learn/dissect_policy.py"      "$PAYLOAD/scripts/"
install -m0644 "$REPO/config/schema-dbus-masked"               "$PAYLOAD/scripts/"
install -m0755 "$HERE/../fedora-kde/scripts/plasma-session-start.sh" "$PAYLOAD/scripts/"
install -m0755 "$HERE/../fedora-kde/scripts/plasmashell-shim"        "$PAYLOAD/scripts/"
install -m0644 "$HERE/../fedora-kde/config/plasma-env/zzz-environment-d.sh" "$PAYLOAD/scripts/"
# XDG autostart runner + plasmashell watchdog + the session env hooks
# plasma-session-start.sh sources from /usr/local/lib/schema/plasma-env.
install -m0755 "$HERE/../fedora-kde/scripts/schema-autostart-runner.sh" "$PAYLOAD/scripts/"
install -m0755 "$HERE/../fedora-kde/scripts/schema-plasma-watchdog.sh"  "$PAYLOAD/scripts/"
install -d "$PAYLOAD/plasma-env"
install -m0644 "$HERE/../fedora-kde/config/plasma-env/"{05-kdedefaults,no-app-scope,ssh-agent-sock}.sh \
    "$HERE/../fedora-kde/config/plasma-workspace/env/zz-schema-autostart.sh" "$PAYLOAD/plasma-env/"
# mock_sd.so fakes /run/systemd/system for plasmashell's sd_booted() probe.
# Compile it on the build host (x86_64, same as target) so the %post chroot,
# which has no toolchain, does not need one.
gcc -shared -fPIC -o "$PAYLOAD/scripts/mock_sd.so" \
    "$HERE/../fedora-kde/scripts/mock_sd.c" -ldl
# The service rail: a Fedora-KDE-correct desktop set (systemd-udevd coldplug +
# fstab mounts + dbus + NetworkManager + schema-logind + polkitd + autologin
# Plasma), boot-proven under schema-init as PID 1. NOT $REPO/services — that
# reference rail's exec paths (schema-udev as udev, lightdm as DM) don't exist
# on a fresh Fedora install and hang the box.
cp -a "$HERE/rail/services/." "$PAYLOAD/services/"

# kernel-install plugin: regenerates a schema-init BLS entry on every dnf kernel
# update (stock systemd entries stay pristine as a boot fallback). %post installs
# it into /usr/lib/kernel/install.d and seeds the first entry.
install -d "$PAYLOAD/kernel-install"
install -m0755 "$REPO/distros/shared/kernel-install/99-schema-init.install" \
    "$PAYLOAD/kernel-install/99-schema-init.install"

echo "=== building the schema-init RPMs from HEAD into the payload ==="
# %post installs these so the box is package-managed and takes later builds
# through dnf. They come from git HEAD (make srpm archives tracked content)
# while the payload copies come from the working tree, so refuse a dirty tree:
# the two would differ.
git -C "$REPO" diff --quiet HEAD -- . ':!tests/livetest/boot-logs' ||
    { echo "uncommitted changes in $REPO: commit them first (the RPMs build from HEAD)" >&2; exit 1; }
RPMTMP="$(dirname "$PAYLOAD")/rpmbuild"
make -C "$REPO" srpm RELDIR="$RPMTMP/srpm" >/dev/null
rpmbuild --rebuild --define "_topdir $RPMTMP" "$RPMTMP"/srpm/*.src.rpm > "$RPMTMP/build.log" 2>&1 ||
    { tail -20 "$RPMTMP/build.log" >&2; echo "RPM build failed" >&2; exit 1; }
install -d "$PAYLOAD/rpms"
for p in "$RPMTMP"/RPMS/x86_64/schema-init-[0-9]*.rpm "$RPMTMP"/RPMS/x86_64/schema-init-daemons-[0-9]*.rpm \
         "$RPMTMP"/RPMS/x86_64/schema-init-session-[0-9]*.rpm; do
    install -m0644 "$p" "$PAYLOAD/rpms/"
done
ls "$PAYLOAD/rpms"

echo "=== injecting kickstart + payload into the ISO ==="
# --add drops the payload tree onto the ISO; the boot media mounts at
# /run/install/repo at install time, but ONLY in the installer environment —
# schema.ks stages it across the chroot with a %post --nochroot copy.
# mkefiboot inside mkksiso needs root; output is then handed back to the user.
sudo mkksiso --ks "$HERE/schema.ks" --add "$PAYLOAD" "$BASE_ISO" "$OUT"
sudo chown "$(id -u):$(id -g)" "$OUT"

echo "=== done: $OUT ==="
echo "Write to USB with:  sudo dd if='$OUT' of=/dev/sdX bs=4M status=progress oflag=direct; sync"
