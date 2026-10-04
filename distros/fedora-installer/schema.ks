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
openssh-server
chrony
pipewire
pipewire-pulseaudio
wireplumber
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
# --- — that was plymouth holding DRM master — but the right default for a
# --- non-systemd init all the same.)
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

# 0. Packages. The schema-init RPMs built from this same tree (payload/rpms)
#    own the files the steps below lay down, so the box is a normal package
#    install, and the COPR repo brings later builds through dnf upgrade (PID 1
#    re-execs onto them in place). If they cannot be installed (no network for
#    their dependencies), the plain file copies below still give a working box,
#    it just will not get updates through dnf.
RPMS=""
if ls "$SRC"/rpms/*.rpm >/dev/null 2>&1 && dnf -y install "$SRC"/rpms/*.rpm; then
    RPMS=1
    cat > /etc/yum.repos.d/_copr:copr.fedorainfracloud.org:ajax80:schema-init.repo <<'EOF'
[copr:copr.fedorainfracloud.org:ajax80:schema-init]
name=Copr repo for schema-init owned by ajax80
baseurl=https://download.copr.fedorainfracloud.org/results/ajax80/schema-init/fedora-$releasever-$basearch/
type=rpm-md
skip_if_unavailable=True
gpgcheck=1
gpgkey=https://download.copr.fedorainfracloud.org/results/ajax80/schema-init/pubkey.gpg
repo_gpgcheck=0
enabled=1
enabled_metadata=1
EOF
else
    echo "WARN: schema-init RPMs not installed; falling back to file copies (no dnf updates)"
fi

# 1. Binaries. usrmerge means /usr/bin is canonical; /sbin etc. resolve to it.
[ -n "$RPMS" ] || install -m0755 "$SRC/bin/schema-init"        /usr/bin/schema-init
# the package puts schema-ctl in /usr/bin; a /usr/local/bin copy would shadow
# every later update of it on PATH
[ -n "$RPMS" ] || install -m0755 "$SRC/bin/schema-ctl" /usr/local/bin/schema-ctl
[ -n "$RPMS" ] || install -m0755 "$SRC/bin/schema-journal-sink" /usr/bin/schema-journal-sink
[ -n "$RPMS" ] || install -m0755 "$SRC/bin/schema-subreaper"   /usr/bin/schema-subreaper
[ -n "$RPMS" ] || install -m0755 "$SRC/bin/schema-board"       /usr/bin/schema-board
[ -n "$RPMS" ] || install -m0755 "$SRC/bin/schema-udev"        /usr/bin/schema-udev   # staged, NOT armed
[ -n "$RPMS" ] || install -m0755 "$SRC/bin/schema-dbus"        /usr/bin/schema-dbus   # dormant until /etc/schema-init/dbus-broker

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
rm -f /etc/schema-init/services/schema-migrate-finish.svc  # migrate-path oneshot; an ISO install has no schema-migrate
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-sysprep.sh" /usr/local/bin/schema-sysprep.sh  # sysprep.svc execs this
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-sshd-start.sh" /usr/local/bin/schema-sshd-start.sh  # sshd.svc execs this
[ -n "$RPMS" ] || install -Dm0755 "$SRC/scripts/schema-sysctl-apply" /usr/libexec/schema-init/schema-sysctl-apply  # sysctl.svc execs this
[ -n "$RPMS" ] || install -Dm0755 "$SRC/scripts/schema-mount-fstab" /usr/libexec/schema-init/schema-mount-fstab  # mount-fstab.svc execs this
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-zram-start.sh" /usr/local/bin/schema-zram-start.sh  # zram.svc execs this

# Desktop-session pipeline (autologin Plasma under schema-init). The rail's
# plasma-autologin.svc drives schema-plasma-autologin.sh, which registers a
# login1 session via schema-logind + the session helpers, then launches Plasma.
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-plasma-autologin.sh" /usr/local/bin/schema-plasma-autologin.sh
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-logind.py"           /usr/local/bin/schema-logind.py
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-doctor.py"           /usr/local/bin/schema-doctor
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-session-register"    /usr/local/bin/schema-session-register
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-session-unregister"  /usr/local/bin/schema-session-unregister
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-dbus-session-run.sh"   /usr/local/bin/schema-dbus-session-run.sh
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/plasma-session-start.sh"    /usr/local/bin/plasma-session-start.sh
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/plasmashell-shim"           /usr/local/bin/plasmashell-shim
install -d /usr/local/lib/schema
[ -n "$RPMS" ] || install -m0644 "$SRC/scripts/zzz-environment-d.sh"      /usr/local/lib/schema/zzz-environment-d.sh
# No systemd --user to run xdg-desktop-autostart.target: the runner sweeps
# ~/.config/autostart (+ xauth cookie, ssh-agent) and starts the plasmashell
# watchdog; plasma-session-start.sh sources plasma-env/*.sh, whose
# zz-schema-autostart.sh fires the runner.
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-autostart-runner.sh" /usr/local/lib/schema/schema-autostart-runner.sh
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/schema-plasma-watchdog.sh"  /usr/local/lib/schema/schema-plasma-watchdog.sh
install -d /usr/local/lib/schema/plasma-env
[ -n "$RPMS" ] || install -m0644 "$SRC/plasma-env/"*.sh /usr/local/lib/schema/plasma-env/
# powerdevil's libddcutil display watcher falls back to POLL mode here and
# burns ~7-9% of a core forever (Eli, DBox). Only external-monitor DDC
# brightness needs it; delete this file to get that back.
install -d /etc/environment.d
printf 'POWERDEVIL_NO_DDCUTIL=1\n' > /etc/environment.d/90-no-ddcutil.conf
install -d /usr/local/lib
[ -n "$RPMS" ] || install -m0755 "$SRC/scripts/mock_sd.so"                 /usr/local/lib/mock_sd.so


# 3. Bootloader: the hook model. Leave the STOCK BLS entries pristine — they
#    boot stock systemd, so a novice always has a working escape hatch in the
#    boot menu if a schema boot ever fails. A SEPARATE schema-init entry per
#    kernel is maintained as the saved default, cloned from the stock entry with
#    init= + enforcing=0 added. The kernel-install plugin regenerates that
#    schema entry on every dnf kernel update, so the box stays on schema-init
#    across updates without pinning — and without ever touching the fallback.
#    (Contrast: rewriting the stock entry in place would leave NO systemd
#    fallback in the menu.)
#    - init=/sbin/schema-init: hand PID 1 to schema-init (added by the plugin).
#    - enforcing=0: kernel-level belt for `selinux --permissive`; lives in
#      kernel-cmdline.d so it lands on the schema entry only. The stock fallback
#      boots permissive via /etc/selinux/config, so it needs no kernel arg.
#    - No boot splash: plymouth + drm are omitted from the initramfs. With them
#      in, dracut-initqueue waits for the udev queue to drain, which includes
#      the GPU driver probe the splash needs (~3.5s on i915 laptops) before root
#      is mounted, and plymouth-switch-root adds ~1s more. Measured on Eli and
#      DBox: kernel stage 11.1->7.0s and 9.1->6.5s. The GPU driver loads after
#      switch-root instead, off the critical chain. Applies to the stock
#      fallback too (same initramfs); systemd boots fine without a splash.

#    Durability seed: a kernel update (dnf) builds the new STOCK BLS entry from
#    /etc/kernel/cmdline when it exists, else from the running cmdline — which on
#    a schema-init box carries init=/sbin/schema-init and would make the "stock"
#    entry boot schema-init too, destroying the fallback. Seed the file from the
#    PRISTINE stock entry now (before any schema entry exists or the default is
#    flipped), so every future stock entry stays pure systemd. The plugin re-adds
#    the schema bits on top for its own entry.
#    grubby reports root= as its OWN field, separate from args= — so the full
#    boot cmdline is `root=<root> <args>`. Capture both; a file missing root=
#    would make a future kernel entry unbootable, which is worse than no seed.
KINFO=$(grubby --info=DEFAULT 2>/dev/null) || true
KROOT=$(printf '%s\n' "$KINFO" | sed -n 's/^root="\(.*\)"$/\1/p' | head -1)
KARGS=$(printf '%s\n' "$KINFO" | sed -n 's/^args="\(.*\)"$/\1/p' | head -1)
if [ -n "$KARGS" ]; then
    if [ -n "$KROOT" ]; then printf 'root=%s %s\n' "$KROOT" "$KARGS" > /etc/kernel/cmdline
    else                    printf '%s\n' "$KARGS"                > /etc/kernel/cmdline
    fi
fi

install -d /etc/dracut.conf.d
printf 'omit_dracutmodules+=" plymouth drm "\n' > /etc/dracut.conf.d/90-schema-no-splash.conf
dracut -f --regenerate-all || echo "WARN: initramfs regenerate failed; splash stays in until next kernel update"

#    Install the kernel-install plugin + its config, then seed the schema entry
#    for the kernel(s) already on disk (the plugin only fires on FUTURE installs).
#    Same path the schema-init package owns, so the first package update takes
#    it over and keeps it current.
[ -n "$RPMS" ] || install -Dm0755 "$SRC/kernel-install/99-schema-init.install" \
    /usr/lib/kernel/install.d/99-schema-init.install

install -d /etc/schema-init/kernel-cmdline.d
printf 'enforcing=0\n' > /etc/schema-init/kernel-cmdline.d/10-enforcing.conf
# boot-default marker: presence tells the plugin to repoint the saved default at
# the newest schema entry on each add — this is what keeps the box on schema-init.
touch /etc/schema-init/boot-default

# Generate the schema entry for each installed kernel, exactly as kernel-install
# would (COMMAND KERNEL_VERSION); each /lib/modules/<ver> is one KERNEL_VERSION.
# Version-sorted so the newest kernel wins the saved default. This also flips the
# default to the schema entry, so it must run AFTER the /etc/kernel/cmdline seed.
for kv in $(ls /lib/modules 2>/dev/null | sort -V); do
    [ -d "/lib/modules/$kv" ] || continue
    SCHEMA_INIT_BIN=/sbin/schema-init \
        /usr/lib/kernel/install.d/99-schema-init.install add "$kv" "/boot/vmlinuz-$kv" \
        || echo "WARN: schema BLS entry for $kv not generated"
done

#    DNS: a stock Fedora install points /etc/resolv.conf at systemd-resolved,
#    which never runs under schema-init -> the file is a dangling symlink, name
#    resolution fails, and NetworkManager reports "limited" connectivity. Have
#    NM own the file directly instead (dns=default writes it, rc-manager=file
#    stops it trying to symlink to resolved).
rm -f /etc/resolv.conf
install -d /etc/NetworkManager/conf.d
printf '[main]\ndns=default\nrc-manager=file\n' > /etc/NetworkManager/conf.d/00-schema-dns.conf

# 4. Stage schema-udev SHADOW: present, LIVE flag disarmed, and record the
#    shipped binary's md5 as THIS ISO's blessed baseline (blakbox's c42164b7
#    baseline is meaningless on someone else's build).
/usr/local/lib/schema/schema-udev-flip-arm.sh disarm || true   # ensure disarmed
SHIP_MD5=$(md5sum /usr/bin/schema-udev | cut -d' ' -f1)
echo "$SHIP_MD5" > /etc/schema-init/schema-udev.ship-md5

# 5. First-boot wizard: install it + a guarded autostart + the headless
#    seatbelt that self-heals a bad flip without a working desktop.
install -m0755 "$SRC/scripts/firstboot-flip-wizard.sh" /usr/local/bin/schema-firstboot-wizard
# the wizard's PRIVILEGED half — it runs as the desktop user (so yad can draw)
# and delegates every root action to this one helper via passwordless sudo.
install -d /usr/local/lib/schema
install -m0755 "$SRC/scripts/schema-flip-apply.sh" /usr/local/lib/schema/schema-flip-apply
install -m0755 "$SRC/scripts/schema-dbus-run.sh"   /usr/local/lib/schema/schema-dbus-run.sh
install -m0755 "$SRC/scripts/schema-dbus-flip.sh"  /usr/local/lib/schema/schema-dbus-flip.sh
install -m0755 "$SRC/scripts/dissect_policy.py"    /usr/local/lib/schema/dissect_policy.py
install -D -m0644 "$SRC/scripts/schema-dbus-masked" /etc/schema-dbus/masked
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

# A clickable Desktop icon too, so the flip is reachable on demand even after the
# autostart popup is dismissed (the wizard removes the autostart once resolved,
# but a novice may want to trigger the flip later). Dropped into the first human
# user's ~/Desktop (the account Anaconda just made). Plasma prompts once to trust
# an executable .desktop on first click — acceptable for a deliberate action.
FBUSER=$(awk -F: '$3>=1000 && $3<65000 {print $1; exit}' /etc/passwd) || true
FBHOME=""
[ -n "$FBUSER" ] && FBHOME=$(getent passwd "$FBUSER" | cut -d: -f6)

# Device access. systemd-logind grants the active session an ACL on /dev/snd,
# /dev/dri, etc. (uaccess); schema-init does not, so the login user needs the
# device groups directly or there is no sound (wireplumber can't open the ALSA
# card -> only a dummy auto_null sink). audio is the one Anaconda leaves off.
[ -n "$FBUSER" ] && usermod -aG audio "$FBUSER" 2>/dev/null || true

# Passwordless sudo for JUST the flip helper, for JUST the login user. The helper
# is the security boundary (a closed set of subcommands, fixed paths); this lets
# the user-side GUI wizard perform the root flip steps without a polkit agent
# (which can't run here — it's a systemd user unit). 0440 + validate before commit.
if [ -n "$FBUSER" ]; then
    printf '%s ALL=(root) NOPASSWD: /usr/local/lib/schema/schema-flip-apply\n' "$FBUSER" \
        > /etc/sudoers.d/schema-flip
    chmod 0440 /etc/sudoers.d/schema-flip
    visudo -cf /etc/sudoers.d/schema-flip || rm -f /etc/sudoers.d/schema-flip
fi

if [ -n "$FBHOME" ] && [ -d "$FBHOME" ]; then
    install -d -o "$FBUSER" -g "$FBUSER" "$FBHOME/Desktop"
    cat > "$FBHOME/Desktop/schema-udev-flip.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Finish Setting Up schema
Comment=Enable schema-udev — the schema-native device manager
Exec=/usr/local/bin/schema-firstboot-wizard
Icon=drive-harddisk
Terminal=false
DESK
    chmod 0755 "$FBHOME/Desktop/schema-udev-flip.desktop"
    chown "$FBUSER:$FBUSER" "$FBHOME/Desktop/schema-udev-flip.desktop"
    # schema-autostart-runner (the stand-in for systemd's xdg-autostart target)
    # sweeps only ~/.config/autostart, so the /etc/xdg entry above never fires
    # under schema-init. Without this the wizard never opens after an armed
    # reboot, nothing confirms, and the seatbelt rolls back a healthy flip.
    install -d -o "$FBUSER" -g "$FBUSER" "$FBHOME/.config" "$FBHOME/.config/autostart"
    install -o "$FBUSER" -g "$FBUSER" -m0644 /etc/xdg/autostart/schema-firstboot.desktop \
        "$FBHOME/.config/autostart/schema-firstboot.desktop"
fi

# headless seatbelt: schema-init oneshot, runs every boot, auto-rolls-back a
# flip that armed but failed to come up healthy (see healthcheck script).
install -m0755 "$SRC/scripts/schema-udev-flip-healthcheck.sh" /usr/local/lib/schema/
cat > /etc/schema-init/services/schema-udev-healthcheck.svc <<'SVC'
name=schema-udev-healthcheck
exec=/usr/local/lib/schema/schema-udev-flip-healthcheck.sh
oneshot=1
start_timeout_sec=300
SVC
install -m0755 "$SRC/scripts/schema-dbus-flip-healthcheck.sh" /usr/local/lib/schema/
cat > /etc/schema-init/services/schema-dbus-healthcheck.svc <<'SVC'
name=schema-dbus-healthcheck
exec=/usr/local/lib/schema/schema-dbus-flip-healthcheck.sh
oneshot=1
start_timeout_sec=300
needs_root=1
critical=0
no_new_privs=0
private_tmp=0
protect_system=0
protect_home=0
SVC

# 6. Hardware video decode for Intel iGPUs (eli-class: Haswell HD 4400). A stock
#    Fedora install ships NO VA-API driver, so Firefox software-decodes every
#    frame and video stutters on a weak CPU (both cores pegged). Fedora dropped
#    the i965 driver that Gen4-Gen8 Intel needs from its repos — it lives in
#    RPM Fusion now; the in-repo iHD driver only covers Gen8+. Install both, gated
#    on an Intel GPU actually being present; libva auto-selects the right one per
#    PCI id. Detect via sysfs vendor (0x8086) so we don't depend on pciutils in
#    the chroot.
INTEL_GPU=no
for _v in /sys/class/drm/card[0-9]/device/vendor; do
    if [ -r "$_v" ] && [ "$(cat "$_v")" = "0x8086" ]; then INTEL_GPU=yes; fi
done
if [ "$INTEL_GPU" = yes ]; then
    echo "=== Intel iGPU detected: enabling VA-API hardware video decode ==="
    dnf install -y "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm" \
        || echo "WARN: rpmfusion-free not enabled; i965 (Haswell-class) decode unavailable"
    dnf install -y libva-intel-driver libva-intel-media-driver libva-utils \
        || echo "WARN: VA-API driver install incomplete"
    # Fedora's stock libavcodec-free has H.264 DECODING stripped (patents), so
    # forcing YouTube to H.264 above without this leaves Firefox with no usable
    # codec at all -> "An error occurred". freeworld restores H.264 decode, which
    # the i965/iHD VA-API path then decodes in hardware.
    dnf install -y libavcodec-freeworld \
        || echo "WARN: libavcodec-freeworld missing; H.264 decode unavailable"
    # Firefox: pin VA-API on and force H.264 — Haswell-class Intel has no VP9/AV1
    # hardware decode (YouTube's defaults), so without this YouTube keeps
    # software-decoding. H.264 has a hardware VLD path on every Intel gen here.
    install -d /etc/firefox/policies
    cat > /etc/firefox/policies/policies.json <<'POL'
{
  "policies": {
    "Preferences": {
      "media.ffmpeg.vaapi.enabled": { "Value": true, "Status": "locked" },
      "media.hardware-video-decoding.force-enabled": { "Value": true, "Status": "locked" },
      "media.mediasource.vp9.enabled": { "Value": false, "Status": "default" },
      "media.av1.enabled": { "Value": false, "Status": "default" }
    }
  }
}
POL
fi

# yad is what the wizard is built on — make sure it's present.
dnf install -y yad || echo "WARN: yad not installed; wizard will not launch"

rm -rf /root/schema-payload
echo "=== schema %post complete ==="
%end
