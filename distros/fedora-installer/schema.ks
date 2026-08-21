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
polkit
NetworkManager
python3-dbus
python3-gobject
%end

# --- Let Anaconda drive its normal GUI for disk/user/timezone — the familiar
# --- Fedora screens Dad clicks through. Deliberately NO autopart/clearpart/
# --- rootpw/user here, so nothing is silently decided for him.

# --- SELinux: permissive. Fedora's stock policy knows only systemd as PID 1;
# --- under enforcing, schema-init's rail (udev coldplug, cgroup writes, runuser
# --- into the session) can hit denials with no matching allow rules. Permissive
# --- keeps the labels and logs AVCs without blocking. A schema-init policy
# --- module is the path back to enforcing later. (Not the first-boot hang cause
# --- — that was plymouth, see the bootloader step below — but the right default
# --- for a non-systemd init all the same.)
selinux --permissive

# --- Stage the ISO payload ACROSS the chroot boundary. The boot media (with the
# --- mkksiso --add tree) is mounted at /run/install/repo in the installer's own
# --- environment ONLY — a chrooted %post cannot see it. So copy it into the new
# --- root here (--nochroot), and the main %post below reads it from inside.
%post --nochroot
cp -a /run/install/repo/schema /mnt/sysroot/root/schema-payload
%end

%post --log=/root/schema-ks-post.log
set -eu
SRC=/root/schema-payload              # staged in by the --nochroot block above
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

# 2. Service rail. gen-services' systemd introspection does NOT work inside an
#    Anaconda %post chroot — there is no running systemd here, so it detects
#    nothing and would leave a rail with no getty/display-manager/network (an
#    unbootable box). So lay down the Fedora-KDE-correct rail from the ISO
#    payload unconditionally (systemd-udevd coldplug + fstab mounts + dbus +
#    NetworkManager + schema-logind + polkitd + autologin Plasma desktop —
#    boot-proven under schema-init as PID 1), then add only THIS machine's
#    fstab-derived mounts on top (a pure /etc/fstab read, which DOES work in
#    the chroot).
install -d /etc/schema-init/services /etc/schema-init/scripts
cp -a "$SRC/services/." /etc/schema-init/services/
rm -f /etc/schema-init/services/*.example        # .svc.example are templates, not live
install -m0755 "$SRC/scripts/schema-sysprep.sh" /usr/local/bin/schema-sysprep.sh  # sysprep.svc execs this

# Desktop-session pipeline (autologin Plasma under schema-init). The rail's
# plasma-autologin.svc drives schema-plasma-autologin.sh, which registers a
# login1 session via schema-logind + the session helpers, then launches Plasma.
install -m0755 "$SRC/scripts/schema-plasma-autologin.sh" /usr/local/bin/schema-plasma-autologin.sh
install -m0755 "$SRC/scripts/schema-logind.py"           /usr/local/bin/schema-logind.py
install -m0755 "$SRC/scripts/schema-session-register"    /usr/local/bin/schema-session-register
install -m0755 "$SRC/scripts/schema-session-unregister"  /usr/local/bin/schema-session-unregister
install -m0755 "$SRC/scripts/plasma-session-start.sh"    /usr/local/bin/plasma-session-start.sh
install -m0755 "$SRC/scripts/plasmashell-shim"           /usr/local/bin/plasmashell-shim
install -d /usr/local/lib
install -m0755 "$SRC/scripts/mock_sd.so"                 /usr/local/lib/mock_sd.so

/usr/local/lib/schema/gen-mounts.sh -o /etc/schema-init 2>/dev/null || true
# mount-fstab.svc runs its script from /usr/local/bin (gen-mounts' fixed path) —
# gen-mounts only writes it under scripts/, so put it where the .svc expects it.
[ -f /etc/schema-init/scripts/mount-fstab.sh ] && \
    install -m0755 /etc/schema-init/scripts/mount-fstab.sh /usr/local/bin/mount-fstab.sh

# 3. Point the bootloader at schema-init. grubby covers grub2 + BLS entries.
#    - init=/sbin/schema-init: hand PID 1 to schema-init.
#    - remove rhgb quiet: THIS is the first-boot hang fix. rhgb starts plymouth,
#      which grabs the DRM master at boot and, on a systemd box, is told to quit
#      by plymouth-quit.service once the DM/desktop is up. schema-init has no
#      such handoff, so plymouthd holds the display forever and kwin never gets
#      it — the box sits on the spinner. Dropping the splash lets the compositor
#      take the display. (Proven: with rhgb quiet present the box hangs even with
#      SELinux permissive; removing it boots straight to the Plasma desktop.)
#    - enforcing=0: kernel-level belt for the permissive config above.
grubby --update-kernel=ALL --args="init=/sbin/schema-init enforcing=0" \
                           --remove-args="rhgb quiet"

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
oneshot=1
SVC

# yad is what the wizard is built on — make sure it's present.
dnf install -y yad || echo "WARN: yad not installed; wizard will not launch"

rm -rf /root/schema-payload
echo "=== schema %post complete ==="
%end
