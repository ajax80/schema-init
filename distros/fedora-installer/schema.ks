# schema.ks — kickstart that turns a stock Fedora 44 install into a
# schema-init system, then hands the user a first-boot wizard.
#
# Injected into the real Fedora 44 ISO with mkksiso (see build-iso.sh), so the
# user sees the ORDINARY Anaconda GUI — same screens as Fedora's own live USB.
# All schema-specific work happens in %post (silent) + a first-boot wizard.
#
# DESIGN STANCE (read before editing):
#   - schema-init as PID 1 is installed unconditionally. It is the safe part:
#     systemd-udev still owns /dev, so a novice always gets a bootable machine.
#   - The schema-udev flip is NOT done here. It is staged (binary present,
#     LIVE flag DISARMED) and offered later by the first-boot wizard, which
#     verifies parity, backs up, arms, reboots, then confirms — with an
#     unattended health-check that auto-rolls-back a bad flip (see wizard).
#
# BASE: Fedora 44 Everything/netinst (Anaconda-native — boots straight into the
# installer, so inst.ks + %post run the standard way). NOT the KDE Live ISO:
# live media has no installer boot entry and mkksiso's mkefiboot chokes on its
# duplicate case-variant EFI files.
#
# Payload: build-iso.sh drops the schema tree at /run/install/repo/schema/ on
# the ISO (mkksiso --add). %post copies from there — no extra RPM repo needed.

# --- Install source. netinst carries no packages of its own; pull from the
# --- Fedora mirrors (needs network at install time — an accepted trade for a
# --- kickstart-automatable base).
url    --mirrorlist="https://mirrors.fedoraproject.org/mirrorlist?repo=fedora-$releasever&arch=$basearch"
repo --name=updates --mirrorlist="https://mirrors.fedoraproject.org/mirrorlist?repo=updates-released-f$releasever&arch=$basearch"

# --- Software: the KDE desktop the installed machine boots into (this is how
# --- a netinst base still yields "just like blakbox" — the desktop comes from
# --- the package set, not the ISO flavor). yad is for the first-boot wizard.
%packages
@^kde-desktop-environment
kernel
grubby
yad
%end

# --- Let Anaconda drive its normal GUI for disk/user/timezone — the familiar
# --- Fedora screens Dad clicks through. Deliberately NO autopart/clearpart/
# --- rootpw/user here, so nothing is silently decided for him.

%post --log=/root/schema-ks-post.log
set -eu
SRC=/run/install/repo/schema          # payload baked onto the ISO
DEST=/                                 # %post is chrooted into the new system

echo "=== schema %post: installing schema-init as PID 1 ==="

# 1. Binaries. usrmerge means /usr/bin is canonical; /sbin etc. resolve to it.
install -m0755 "$SRC/bin/schema-init"        /usr/bin/schema-init
install -m0755 "$SRC/bin/schema-ctl"         /usr/local/bin/schema-ctl
install -m0755 "$SRC/bin/schema-journal-sink" /usr/bin/schema-journal-sink
install -m0755 "$SRC/bin/schema-subreaper"   /usr/bin/schema-subreaper
install -m0755 "$SRC/bin/schema-board"       /usr/bin/schema-board
install -m0755 "$SRC/bin/schema-udev"        /usr/bin/schema-udev   # staged, NOT armed

# flip tooling + parity gates the wizard calls
install -d /usr/local/lib/schema
install -m0755 "$SRC/scripts/schema-udev-flip-arm.sh"    /usr/local/lib/schema/
install -m0755 "$SRC/scripts/schema-udev-flip-backup.sh" /usr/local/lib/schema/
install -m0755 "$SRC/bin/verify-rules-live"              /usr/local/lib/schema/
install -m0755 "$SRC/scripts/gen-services.sh"            /usr/local/lib/schema/
install -m0755 "$SRC/scripts/gen-mounts.sh"              /usr/local/lib/schema/

# 2. Service rail for THIS machine. gen-services.sh/gen-mounts.sh must run while
#    the system is STILL systemd — which %post is (schema-init only takes over
#    after step 3 + reboot). They read the enabled systemd units and the target
#    /etc/fstab and emit .svc stubs so the box comes up running what it ran
#    before. Generic rail is the fallback if offline detection comes up thin.
install -d /etc/schema-init/services /etc/schema-init/scripts
/usr/local/lib/schema/gen-services.sh -o /etc/schema-init 2>/dev/null || true
/usr/local/lib/schema/gen-mounts.sh   -o /etc/schema-init 2>/dev/null || true
# guarantee a non-empty rail no matter what detection produced
if [ -z "$(ls -A /etc/schema-init/services 2>/dev/null)" ]; then
    echo "detection produced no rail; installing generic services"
    cp -a "$SRC/services/." /etc/schema-init/services/
fi

# 3. Point the bootloader at schema-init. grubby covers grub2 + BLS entries.
grubby --update-kernel=ALL --args="init=/sbin/schema-init"

# 4. Stage schema-udev SHADOW: present, LIVE flag disarmed, and record the
#    shipped binary's md5 as THIS ISO's blessed baseline (blakbox's c42164b7
#    baseline is meaningless on someone else's build).
/usr/local/lib/schema/schema-udev-flip-arm.sh disarm || true   # ensure disarmed
SHIP_MD5=$(md5sum /usr/bin/schema-udev | cut -d' ' -f1)
echo "$SHIP_MD5" > /etc/schema-init/schema-udev.ship-md5

# 5. First-boot wizard: install it + a guarded autostart + the headless
#    seatbelt that self-heals a bad flip without a working desktop.
install -m0755 "$SRC/scripts/firstboot-flip-wizard.sh" /usr/local/bin/schema-firstboot-wizard
install -d /etc/xdg/autostart
cat > /etc/xdg/autostart/schema-firstboot.desktop <<'DESK'
[Desktop Entry]
Type=Application
Name=Finish Setting Up schema
Exec=/usr/local/bin/schema-firstboot-wizard
OnlyShowIn=GNOME;KDE;
X-GNOME-Autostart-enabled=true
NoDisplay=false
DESK

# headless seatbelt: schema-init oneshot, runs every boot, auto-rolls-back a
# flip that armed but failed to come up healthy (see healthcheck script).
install -m0755 "$SRC/scripts/schema-udev-flip-healthcheck.sh" /usr/local/lib/schema/
cat > /etc/schema-init/services/schema-udev-healthcheck.svc <<'SVC'
name=schema-udev-healthcheck
exec=/usr/local/lib/schema/schema-udev-flip-healthcheck.sh
oneshot=yes
SVC

# yad is what the wizard is built on — make sure it's present.
dnf install -y yad || echo "WARN: yad not installed; wizard will not launch"

echo "=== schema %post complete ==="
%end
