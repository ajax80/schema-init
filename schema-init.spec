Name:           schema-init
Version:        0.1.1
Release:        1%{?dist}
Summary:        Minimal PID 1 init system driven by a weight-state machine

License:        AGPL-3.0-or-later
URL:            https://github.com/ajax80/schema-init
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  glibc-static

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
# The Makefile sets its own CFLAGS (-std=c99 -D_GNU_SOURCE) and links -static.
# %%{optflags} is deliberately not injected: overriding CFLAGS from the command
# line replaces those flags rather than adding to them, which breaks the build.
%make_build

%install
%make_install PREFIX=%{_prefix} SYSCONFDIR=%{_sysconfdir}

%files
%license LICENSE
%doc README.md docs/
%{_bindir}/schema-init
%{_bindir}/schema-ctl
%{_bindir}/schema-subreaper
%{_bindir}/schema-journal-sink
%dir %{_sysconfdir}/%{name}
%dir %{_sysconfdir}/%{name}/services
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/services

%changelog
* Fri Jul 24 2026 Jonathan Ayers <ayersjon80@gmail.com> - 0.1.1-1
- Initial RPM package
- Adds the AGPL-3.0 license text, which v0.1.0 shipped without
