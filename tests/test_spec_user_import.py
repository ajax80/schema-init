#!/usr/bin/env python3
"""schema-import --user packaging: importer in base libexec, default user-skip."""
import os, re, sys
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
read = lambda p: open(os.path.join(REPO, p)).read()
SPEC, MK = read("schema-init.spec"), read("Makefile")
KS, ISO = read("distros/fedora-installer/schema.ks"), read("distros/fedora-installer/build-iso.sh")

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

base = re.search(r"^%files\n(.*?)(?=^%files|^%changelog|\Z)", SPEC, re.S | re.M).group(1)
check("base %files ships libexec importer", "%{_libexecdir}/schema-init/schema-import\n" in base)
check("base %files ships user-skip noreplace",
      "%config(noreplace) %{_sysconfdir}/schema-init/user-skip\n" in base)
check("base %files ships /usr/bin/schema-import (no -migrate needed)", "%{_bindir}/schema-import\n" in base)
check("Makefile links /usr/bin/schema-import to the libexec importer",
      "ln -sf ../libexec/schema-init/schema-import $(DESTDIR)$(BINDIR)/schema-import" in MK)
check("Makefile installs libexec importer",
      "distros/fedora-installer/migrate/schema-import.py $(DESTDIR)$(PREFIX)/libexec/schema-init/schema-import" in MK)
check("Makefile installs user-skip", "config/user-skip $(DESTDIR)$(SYSCONFDIR)/schema-init/user-skip" in MK)
check("ISO stages importer + user-skip",
      "schema-import.py" in ISO and "config/user-skip" in ISO)
check("ks fallback installs importer", "/usr/libexec/schema-init/schema-import" in KS)
check("ks fallback installs user-skip", "/etc/schema-init/user-skip" in KS)
names = [l.strip() for l in read("config/user-skip").splitlines() if l.strip() and not l.lstrip().startswith("#")]
check("default skip list", names == ["pipewire.service", "pipewire.socket", "pipewire-pulse.service",
                                     "pipewire-pulse.socket", "wireplumber.service", "syncthing.service"])

# PR E2: the session-bus systemd1 provider ships in -session; routed activation dir first
sess = re.search(r"^%files session\n(.*?)(?=^%package|^%files|\Z)", SPEC, re.S | re.M).group(1)
sess_pkg = re.search(r"^%package session\n(.*?)(?=^%description)", SPEC, re.S | re.M).group(1)
check("-session ships schema-systemd1-session", "/usr/local/bin/schema-systemd1-session\n" in sess)
check("-session installs its activation file",
      "dbus-1/services distros/fedora-kde/config/dbus-1/services/org.freedesktop.systemd1.service" in SPEC)
check("-session requires python3-dbus + gobject",
      "python3-dbus" in sess_pkg and "python3-gobject-base" in sess_pkg)
act = read("distros/fedora-kde/config/dbus-1/services/org.freedesktop.systemd1.service")
check("activation file execs the packaged shim", "Exec=/usr/local/bin/schema-systemd1-session\n" in act)
run = read("scripts/schema-dbus-session-run.sh")
check("schema-dbus searches schema-user's routed dir first",
      'SCHEMA_DBUS_SVCDIRS="$XDG_RUNTIME_DIR/schema-user/dbus-services:' in run)
check("stock dbus-daemon fallback searches it first too",
      run.index("<servicedir>$XDG_RUNTIME_DIR/schema-user/dbus-services</servicedir>")
      < run.index("<servicedir>$HOME/.local/share/dbus-1/services</servicedir>"))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
