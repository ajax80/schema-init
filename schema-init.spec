Name:           schema-init
Version:        0.4.0
Release:        1%{?snapshot:.%{snapshot}}%{?dist}
Summary:        Minimal PID 1 init system driven by a weight-state machine

License:        AGPL-3.0-or-later
URL:            https://github.com/ajax80/schema-init
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  glibc-static
BuildRequires:  libacl-devel
BuildRequires:  dbus-devel
BuildRequires:  libzstd-devel
BuildRequires:  pkgconf-pkg-config
Requires:       python3
# ISO-installed and migrated boxes run both daemons; an upgrade must pull them.
Requires:       %{name}-daemons = %{version}-%{release}
Recommends:     python3-dbus
Recommends:     python3-gobject-base

# Only these two are built and tested. COPR has no armv7hl target; 32-bit ARM
# is a manual cross-build via `make armhf`.
ExclusiveArch:  x86_64 aarch64

%description
schema-init is a PID 1 init system for Linux that supervises services through a
weight-state machine instead of unit files and dependency graphs. It mounts
pseudo-filesystems, spawns services in dependency order, reaps children and
supervises restarts with bounded backoff. There is no journal daemon, no
socket-activation engine and no D-Bus event loop; PID 1 is a single statically
linked binary holding a few MB of RSS in one thread.

Installing this package does NOT change your init system. It only places the
binaries and reference service files on disk. Booting schema-init is an
explicit, manual step: you add init=/sbin/schema-init to a kernel command line
yourself. Read the "Replacing a running init" and "GRUB setup" sections of the
README before you do — a broken PID 1 is a machine that will not boot. Keep a
working systemd boot entry.

Reference .svc and .grp files are installed to %{_datadir}/%{name}/services as
examples. They are deliberately NOT installed into %{_sysconfdir}/%{name},
which is created empty, so that installing or upgrading this package can never
overwrite a service file a running system depends on.

%prep
%autosetup

%build
# The Makefile takes CFLAGS from the environment and appends the flags the code
# requires, so rpm's %%{optflags} -- fortify, stack-protector, annobin -- apply
# to the dynamically linked helpers. PID 1 itself is linked -static and does not
# consume LDFLAGS; the hardened-ld specs would force PIE onto a static binary.
#
# schema-udev (the native device manager) lives in the same tree but is a
# separate, still-maturing concern that is deliberately NOT shipped by the
# base package -- installing schema-init must not change your init system,
# but retiring systemd-udevd is a manual, hardware-specific cutover, not a
# package upgrade. It is built here (migrate_bins) only so the -migrate
# subpackage below can ship it; the base %%files list never references it.
%global core_bins schema-init schema-ctl schema-subreaper schema-journal-sink schema-board schema-coredump
%global migrate_bins schema-udev verify-rules-live schema-systemctl schema-dbus
%make_build BINS="%{core_bins} %{migrate_bins}" SCHEMA_VERSION="%{version}-%{release}"
gcc %{optflags} -shared -fPIC -o mock_sd.so distros/fedora-kde/scripts/mock_sd.c -ldl %{build_ldflags}

%install
%make_install PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir} BINS="%{core_bins}"
make install-migrate DESTDIR=%{buildroot} PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir}
make install-wizard DESTDIR=%{buildroot} PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir}
install -Dm0755 scripts/schema-bootok %{buildroot}%{_bindir}/schema-bootok
install -Dm0755 scripts/schema-doctor.py %{buildroot}/usr/local/bin/schema-doctor
install -Dm0755 scripts/schema-logind.py %{buildroot}/usr/local/bin/schema-logind.py
install -Dm0755 scripts/schema-session-register %{buildroot}/usr/local/bin/schema-session-register
install -Dm0755 scripts/schema-session-unregister %{buildroot}/usr/local/bin/schema-session-unregister
install -Dm0755 scripts/schema-sysctl-apply %{buildroot}%{_libexecdir}/schema-init/schema-sysctl-apply
install -Dm0755 distros/fedora-installer/rail/scripts/schema-sysprep.sh %{buildroot}/usr/local/bin/schema-sysprep.sh
install -Dm0755 distros/fedora-installer/rail/scripts/schema-sshd-start.sh %{buildroot}/usr/local/bin/schema-sshd-start.sh
install -Dm0755 distros/fedora-installer/rail/scripts/schema-zram-start.sh %{buildroot}/usr/local/bin/schema-zram-start.sh
install -Dm0755 scripts/09_schema_fallback %{buildroot}%{_sysconfdir}/grub.d/09_schema_fallback
install -Dm0755 distros/fedora-kde/scripts/schema-plasma-autologin.sh %{buildroot}/usr/local/bin/schema-plasma-autologin.sh
install -Dm0755 distros/fedora-kde/scripts/plasma-session-start.sh %{buildroot}/usr/local/bin/plasma-session-start.sh
install -Dm0755 distros/fedora-kde/scripts/plasmashell-shim %{buildroot}/usr/local/bin/plasmashell-shim
install -Dm0755 distros/fedora-kde/scripts/schema-autostart-runner.sh %{buildroot}/usr/local/lib/schema/schema-autostart-runner.sh
install -Dm0755 distros/fedora-kde/scripts/schema-plasma-watchdog.sh %{buildroot}/usr/local/lib/schema/schema-plasma-watchdog.sh
install -Dm0755 scripts/schema-dbus-session-run.sh %{buildroot}/usr/local/bin/schema-dbus-session-run.sh
install -Dm0755 mock_sd.so %{buildroot}/usr/local/lib/mock_sd.so
install -Dm0644 distros/fedora-kde/config/plasma-env/zzz-environment-d.sh %{buildroot}/usr/local/lib/schema/zzz-environment-d.sh
install -Dm0644 -t %{buildroot}/usr/local/lib/schema/plasma-env distros/fedora-kde/config/plasma-env/{05-kdedefaults,no-app-scope,ssh-agent-sock}.sh distros/fedora-kde/config/plasma-workspace/env/zz-schema-autostart.sh

%package daemons
Summary:   schema-udev and schema-dbus, schema-init's udev and D-Bus daemons
Requires:  %{name} = %{version}-%{release}
Conflicts: %{name}-migrate < %{version}-%{release}
%description daemons
The reclaimed udev daemon and D-Bus broker. A box whose udev or D-Bus was
flipped onto them (ISO install, migrate wizard) needs these kept current;
installing them changes nothing until a flip points a service at them.
The base package requires this one so a plain upgrade keeps them current.

%post daemons
md5sum %{_bindir}/schema-udev | cut -d' ' -f1 > %{_sysconfdir}/schema-init/schema-udev.ship-md5

%postun daemons
if [ $1 -eq 0 ]; then
    rm -f %{_sysconfdir}/schema-init/schema-udev.ship-md5
fi

%files daemons
%{_bindir}/schema-udev
%{_bindir}/schema-dbus
%ghost %{_sysconfdir}/schema-init/schema-udev.ship-md5

%package session
Summary:   Autologin Plasma session pipeline for installer-built boxes
Requires:  %{name} = %{version}-%{release}
Requires:  %{name}-daemons = %{version}-%{release}
%description session
The Fedora KDE desktop session the installer ISO lays down: the autologin
launcher, the session-bus wrapper, the direct kwin/plasmashell session start,
the XDG-autostart runner, the plasmashell watchdog and the session env hooks.
Not pulled in by the base package: a box running its own session scripts
keeps them. Removing this package removes these files, and a rail whose
plasma-autologin.svc execs them loses its desktop. The session bus runs
schema-dbus only when /etc/schema-init/dbus-broker exists, stock
dbus-daemon otherwise.

%files session
/usr/local/bin/schema-plasma-autologin.sh
/usr/local/bin/schema-dbus-session-run.sh
/usr/local/bin/plasma-session-start.sh
/usr/local/bin/plasmashell-shim
/usr/local/lib/mock_sd.so
%dir /usr/local/lib/schema
%dir /usr/local/lib/schema/plasma-env
/usr/local/lib/schema/zzz-environment-d.sh
/usr/local/lib/schema/schema-autostart-runner.sh
/usr/local/lib/schema/schema-plasma-watchdog.sh
/usr/local/lib/schema/plasma-env/*.sh

%package migrate
Summary:   Guided in-place Fedora KDE onboarding onto schema-init (prebuilt)
Requires:  %{name} = %{version}-%{release}
Requires:  %{name}-daemons = %{version}-%{release}
Requires:  python3
Requires:  btrfs-progs
%description migrate
Prebuilt engine that converts a running Fedora KDE box onto schema-init in
place across two reboots, keeping a systemd fallback boot entry. Drives the
foundation flip (schema-init PID 1) and the desktop-seam flip (schema-udev)
from the schema-migrate CLI. Front-ended by schema-wizard. The D-Bus broker
(schema-dbus) ships dormant: both launchers run stock dbus-daemon until
/etc/schema-init/dbus-broker exists, and nothing here creates it.

%post migrate
if alternatives --display systemctl >/dev/null 2>&1; then
    alternatives --install /usr/bin/systemctl systemctl %{_bindir}/schema-systemctl 100
else
    if [ ! -L /usr/bin/systemctl ] && [ -f /usr/bin/systemctl ]; then
        mv /usr/bin/systemctl /usr/bin/systemctl.real
    fi
    ln -sf %{_bindir}/schema-systemctl /usr/bin/systemctl
fi

%postun migrate
if [ $1 -eq 0 ]; then
    if alternatives --display systemctl >/dev/null 2>&1; then
        alternatives --remove systemctl %{_bindir}/schema-systemctl
    elif [ -f /usr/bin/systemctl.real ]; then
        rm -f /usr/bin/systemctl
        mv /usr/bin/systemctl.real /usr/bin/systemctl
    elif [ "$(readlink /usr/bin/systemctl 2>/dev/null)" = "%{_bindir}/schema-systemctl" ]; then
        rm -f /usr/bin/systemctl
    fi
fi

%transfiletriggerin migrate -- /usr/bin/systemctl
if [ ! -L /usr/bin/systemctl ] && [ -f /usr/bin/systemctl ]; then
    mv -f /usr/bin/systemctl /usr/bin/systemctl.real
    ln -sf %{_bindir}/schema-systemctl /usr/bin/systemctl
fi

%transfiletriggerin migrate -- /usr/lib/systemd/system
%{_bindir}/schema-import || :

%files migrate
%dir %{_libexecdir}/schema-init
%dir %{_datadir}/%{name}/migrate
%{_bindir}/schema-migrate
%{_bindir}/schema-systemctl
%{_bindir}/schema-import
%{_libexecdir}/schema-init/schema-flip-apply
%{_libexecdir}/schema-init/schema-udev-flip-arm.sh
%{_libexecdir}/schema-init/schema-udev-flip-backup.sh
%{_libexecdir}/schema-init/schema-udev-flip-healthcheck.sh
%{_libexecdir}/schema-init/verify-rules-live
%{_libexecdir}/schema-init/schema-dbus-run.sh
%{_libexecdir}/schema-init/schema-dbus-flip.sh
%{_libexecdir}/schema-init/schema-dbus-flip-healthcheck.sh
%{_libexecdir}/schema-init/dissect_policy.py
%{_libexecdir}/schema-init/stage.py
%{_datadir}/%{name}/migrate/prevent-set.list
%{_datadir}/%{name}/migrate/distros
%{_datadir}/%{name}/migrate/scripts
%config(noreplace) %{_sysconfdir}/sudoers.d/schema-wizard
%dir %{_sysconfdir}/schema-dbus
%config(noreplace) %{_sysconfdir}/schema-dbus/masked

%package wizard
Summary:   Guided PySide6 GUI that converts a Fedora KDE box onto schema-init
Requires:  %{name}-migrate = %{version}-%{release}
Requires:  python3-pyside6
BuildArch: noarch
%description wizard
The onboarding wizard: a native Plasma (Qt/QML) GUI that walks a novice through
the two-reboot in-place conversion, driving the migrate engine, the flip helper,
and schema-doctor. Unprivileged; escalates only through the fixed helpers.

%files wizard
%dir %{_libexecdir}/schema-init/wizard
%dir %{_libexecdir}/schema-init/wizard/qml
%{_bindir}/schema-wizard
%{_libexecdir}/schema-init/wizard/*.py
%{_libexecdir}/schema-init/wizard/qml/*.qml
%{_sysconfdir}/xdg/autostart/schema-wizard.desktop

%pre
# The running login1 stub re-execs itself on SIGHUP, keeping its pid and the
# DRM/VT fds the desktop session depends on. Stubs older than that handoff
# die on SIGHUP instead, so only reload one whose file knows the signal.
if grep -qs SIGHUP /usr/local/bin/schema-logind.py; then
    touch /run/schema-logind.hup-ok 2>/dev/null || :
fi

%post
if [ -e /run/schema-logind.hup-ok ]; then
    rm -f /run/schema-logind.hup-ok
    pkill -HUP -f '^(/usr/bin/)?python3(\.[0-9]+)? /usr/local/bin/schema-logind\.py( |$)' || :
fi
if [ $1 -ge 2 ]; then
    touch /run/schema-init.reexec-pending 2>/dev/null || :
fi

%posttrans
# PID 1 swaps itself onto the upgraded binary in place; a refusal (memory
# pressure, a binary that fails its dry run, a PID 1 too old to know the
# verb) leaves the running one untouched and never fails the transaction.
if [ -e /run/schema-init.reexec-pending ]; then
    rm -f /run/schema-init.reexec-pending
    if [ "$(cat /proc/1/comm 2>/dev/null)" = schema-init ] && [ /proc/1/root -ef / ] && [ -S /run/schema-init.sock ]; then
        %{_bindir}/schema-ctl reexec || echo "schema-init: PID 1 kept its old binary; run 'schema-ctl reexec' or reboot to pick up the upgrade"
    fi
fi

%files
%license LICENSE
%doc README.md docs/
%{_bindir}/schema-init
%{_bindir}/schema-ctl
%{_bindir}/schema-subreaper
%{_bindir}/schema-journal-sink
%{_bindir}/schema-board
%{_bindir}/schema-coredump
%{_bindir}/schema-snapshot
%{_bindir}/schema-bootok
/usr/local/bin/schema-doctor
/usr/local/bin/schema-logind.py
/usr/local/bin/schema-session-register
/usr/local/bin/schema-session-unregister
/usr/local/bin/schema-sysprep.sh
/usr/local/bin/schema-sshd-start.sh
/usr/local/bin/schema-zram-start.sh
%dir %{_libexecdir}/schema-init
%{_libexecdir}/schema-init/schema-sysctl-apply
%{_sysconfdir}/grub.d/09_schema_fallback
%dir %{_sysconfdir}/%{name}
%dir %{_sysconfdir}/%{name}/services
%config(noreplace) %{_sysconfdir}/logrotate.d/%{name}
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/services

%changelog
* Sun Sep 27 2026 Jonathan Ayers <44883767+ajax80@users.noreply.github.com> - 0.4.0-1
- PID 1 replaces its own binary in place (schema-ctl reexec), and an
  upgrade does it automatically: services keep running, no reboot
- schema-ctl exits 1 when PID 1 refuses a command
- The migrate wizard and the ISO first-boot wizard offer the schema-dbus
  switch as a separate, optional last step, with a headless seatbelt that
  rolls back to dbus-daemon and reboots if the broker does not take
- -migrate ships schema-dbus, its launcher and the flip helpers
- schema-dbus: stricter message validation and bounded per-peer resources
- The udev switch actually takes on a migrated box; both seatbelts wait
  on boot uptime (not the RTC) and say so on screen while they wait
- Migrated boxes keep DNS under schema-init, including after booting the
  stock entry; Plasma session support and wizard autostart are installed
- Every --advanced-* opt-out is acted on; the btrfs pre-change snapshot is
  recorded, named on the recovery card and removed by --uninstall
- systemctl shim passes through to the real systemctl while PID 1 is
  systemd, and reboot/poweroff/halt reach schema-ctl under schema-init
- Service hardening: mount-namespace isolation, per-host default switch
  (off by default), hardened reference .svc files
- exec= with inline arguments is refused by both parsers
- login1 stub accepts SetWallMessage
- The login1 stub and its session register/unregister helpers ship in the
  package; an update reloads the running stub in place with SIGHUP
- schema-udev and schema-dbus move to a -daemons subpackage, so ISO-installed
  boxes can keep them current without the -migrate systemctl shim
- sysctl.d is re-applied after switch-root (sysctl.svc on installed and
  migrated boxes), then core_pattern is taken back for schema-coredump
- The installer rail's sysprep, sshd and zram start scripts ship in the
  package, so installed boxes get fixes to them on update
- New -session subpackage ships the installer's Plasma autologin session
  pipeline; nothing requires it, so hand-built sessions are left alone

* Fri Sep 25 2026 Jonathan Ayers <44883767+ajax80@users.noreply.github.com> - 0.3.1-1
- kernel-install plugin finds the GRUB BLS entries when kernel-install resolves
  BOOT_ROOT to the ESP, so dnf kernel updates get a schema-init entry again
  instead of silently staying on the install-time kernel
- Installer ships the XDG autostart runner, plasmashell watchdog and session env
  hooks system-wide, so ~/.config/autostart apps start on installed boxes
- Autostart runner no longer waits 20s for an Xwayland cookie Plasma 6 never
  creates, and logs "no ssh key" instead of a false ssh-add failure
- Spec version catches up with the v0.3.0 installer release

* Tue Sep 01 2026 Jonathan Ayers <44883767+ajax80@users.noreply.github.com> - 0.2.1-1
- Hardware watchdog: PID 1 loads the sp5100_tco module itself and arms
  /dev/watchdog0, so boxes without an initramfs still get a hardware watchdog
- Memory-pressure reclaim: a service's cgroup path and freeze state now survive
  a schema-ctl reload, so freeze/reclaim keeps working after a reload instead of
  silently becoming a no-op
- cgroup efficiency: io.weight / cpu.idle shielding keeps non-critical services
  off the critical path
- init: gracefully stops container cgroups during the shutdown sweep, applies
  /etc/hostname at boot, and raises PID 1's own RLIMIT_NOFILE while keeping
  children off the raised limit
- Service hardening: opt-in no_new_privs and a keep_caps bounding set
  (chronyd hardened as the reference service)
- Timers: on_calendar day-of-week and day-of-month; a completed run-once boot
  timer stays terminal across a reload
- schema-ctl: reports a rejected reload instead of answering ok; guards a
  strncat length against a size_t underflow
- args= values are left-trimmed of leading whitespace
- Per-service boot-timing cost column, sorted by the critical path
- rail: persists service_log lines to rail.log and logs excise/recovery
  transitions on a start-timeout
- logrotate: rotates daily, covers sddm-schema.log, and arms the timer
  persistently
- schema-board: increment 3 -- apply a card to a lit slot

* Mon Jul 27 2026 Jonathan Ayers <44883767+ajax80@users.noreply.github.com> - 0.1.3-1
- PID 1 resets the child signal mask before every exec, so services no longer
  inherit a blocked SIGCHLD and can reap their own children
- Shutdown no longer blocks PID 1 on a console write, bounds sync, and kills
  what the cgroup sweep misses; the transcript is persisted
- The remount sweep no longer takes / read-only before the log is written
- VT switches are mediated with VT_PROCESS, so consoles repaint on a graphical
  session; IXON is disarmed on the mediated session VT

* Fri Jul 24 2026 Jonathan Ayers <44883767+ajax80@users.noreply.github.com> - 0.1.2-1
- Ship schema-board, the read-only weight-state board; needs no root
- Ship a logrotate config and an example rotation timer
- schema-ctl gains --help/--version and reports the real connect error

* Fri Jul 24 2026 Jonathan Ayers <44883767+ajax80@users.noreply.github.com> - 0.1.1-1
- Initial RPM package
- Adds the AGPL-3.0 license text, which v0.1.0 shipped without
