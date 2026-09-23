Name:           schema-init
Version:        0.2.1
Release:        1%{?dist}
Summary:        Minimal PID 1 init system driven by a weight-state machine

License:        AGPL-3.0-or-later
URL:            https://github.com/ajax80/schema-init
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  glibc-static
BuildRequires:  libacl-devel

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
%global core_bins schema-init schema-ctl schema-subreaper schema-journal-sink schema-board
%global migrate_bins schema-udev verify-rules-live schema-systemctl
%make_build BINS="%{core_bins} %{migrate_bins}"

%install
%make_install PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir} BINS="%{core_bins}"
make install-migrate DESTDIR=%{buildroot} PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir}
make install-wizard DESTDIR=%{buildroot} PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir}

%package migrate
Summary:   Guided in-place Fedora KDE onboarding onto schema-init (prebuilt)
Requires:  %{name} = %{version}-%{release}
Requires:  python3
Requires:  btrfs-progs
%description migrate
Prebuilt engine that converts a running Fedora KDE box onto schema-init in
place across two reboots, keeping a systemd fallback boot entry. Drives the
foundation flip (schema-init PID 1) and the desktop-seam flip (schema-udev +
schema-dbus) from the schema-migrate CLI. Front-ended by schema-wizard.

%post migrate
md5sum %{_bindir}/schema-udev | cut -d' ' -f1 > %{_sysconfdir}/schema-init/schema-udev.ship-md5
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
    rm -f %{_sysconfdir}/schema-init/schema-udev.ship-md5
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

%files migrate
%dir %{_libexecdir}/schema-init
%dir %{_datadir}/%{name}/migrate
%{_bindir}/schema-migrate
%{_bindir}/schema-udev
%{_bindir}/schema-systemctl
%{_bindir}/schema-import
%{_libexecdir}/schema-init/schema-flip-apply
%{_libexecdir}/schema-init/schema-udev-flip-arm.sh
%{_libexecdir}/schema-init/schema-udev-flip-backup.sh
%{_libexecdir}/schema-init/schema-udev-flip-healthcheck.sh
%{_libexecdir}/schema-init/verify-rules-live
%{_libexecdir}/schema-init/stage.py
%{_libexecdir}/schema-init/schema-doctor
%{_datadir}/%{name}/migrate/prevent-set.list
%{_datadir}/%{name}/migrate/distros
%{_datadir}/%{name}/migrate/scripts
%ghost %{_sysconfdir}/schema-init/schema-udev.ship-md5
%config(noreplace) %{_sysconfdir}/sudoers.d/schema-wizard

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

%files
%license LICENSE
%doc README.md docs/
%{_bindir}/schema-init
%{_bindir}/schema-ctl
%{_bindir}/schema-subreaper
%{_bindir}/schema-journal-sink
%{_bindir}/schema-board
%{_bindir}/schema-snapshot
%dir %{_sysconfdir}/%{name}
%dir %{_sysconfdir}/%{name}/services
%config(noreplace) %{_sysconfdir}/logrotate.d/%{name}
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/services

%changelog
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
