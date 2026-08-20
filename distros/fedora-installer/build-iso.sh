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
make -C "$REPO" schema-init schema-ctl schema-journal-sink schema-subreaper schema-board schema-udev >/dev/null
make -C "$REPO" verify-rules-live >/dev/null 2>&1 || true   # if it has a make target

echo "=== staging payload the %post copies from (ISO:/schema) ==="
mkdir -p "$PAYLOAD/bin" "$PAYLOAD/scripts" "$PAYLOAD/services"
for b in schema-init schema-ctl schema-journal-sink schema-subreaper schema-board schema-udev verify-rules-live; do
    install -m0755 "$REPO/$b" "$PAYLOAD/bin/$b"
done
install -m0755 "$REPO/scripts/schema-udev-flip-arm.sh"    "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/schema-udev-flip-backup.sh" "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/gen-services.sh"            "$PAYLOAD/scripts/"
install -m0755 "$REPO/scripts/gen-mounts.sh"              "$PAYLOAD/scripts/"
install -m0755 "$HERE/firstboot-flip-wizard.sh"           "$PAYLOAD/scripts/"
install -m0755 "$HERE/schema-udev-flip-healthcheck.sh"    "$PAYLOAD/scripts/"
# a generic service rail as fallback if --generate-profile can't detect hardware
cp -a "$REPO/services/." "$PAYLOAD/services/" 2>/dev/null || true

echo "=== injecting kickstart + payload into the ISO ==="
# --add drops the payload tree onto the ISO; it lands under /run/install/repo
# at install time, which schema.ks reads as $SRC.
# mkefiboot inside mkksiso needs root; output is then handed back to the user.
sudo mkksiso --ks "$HERE/schema.ks" --add "$PAYLOAD" "$BASE_ISO" "$OUT"
sudo chown "$(id -u):$(id -g)" "$OUT"

echo "=== done: $OUT ==="
echo "Write to USB with:  sudo dd if='$OUT' of=/dev/sdX bs=4M status=progress oflag=direct; sync"
