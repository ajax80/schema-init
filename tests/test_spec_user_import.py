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
check("migrate keeps /usr/bin/schema-import", "%{_bindir}/schema-import\n" in SPEC)
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

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
