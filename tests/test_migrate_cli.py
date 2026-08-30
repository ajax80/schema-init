#!/usr/bin/env python3
"""CLI sequencing tests — full deploy against a temp root with injected runner."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
fakerepo = tempfile.mkdtemp(); os.environ["MIGRATE_REPO"] = fakerepo
os.environ["MIGRATE_KERNEL"] = "6.10.0"
# minimal system + repo
for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "etc/os-release"), "w").write("ID=fedora\n")
open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
open(os.path.join(root, "etc/fstab"), "w").write("UUID=aaa / ext4 defaults 0 1\n")
open(os.path.join(root, "etc/passwd"), "w").write("jandoe:x:1000:1000::/home/jandoe:/bin/bash\n")
open(os.path.join(root, "boot/loader/entries/f.conf"), "w").write(
    "title Fedora\nversion 6.10.0\noptions root=UUID=aaa ro\n")
for d in ("distros/fedora-kde/scripts", "distros/fedora-installer/rail/services",
          "distros/fedora-installer/migrate"):
    os.makedirs(os.path.join(fakerepo, d))
open(os.path.join(fakerepo, "distros/fedora-kde/scripts/plasma-session-start.sh"), "w").write("#!/bin/sh\n")
open(os.path.join(fakerepo, "distros/fedora-installer/rail/services/dbus.svc"), "w").write("name=dbus\n")
open(os.path.join(fakerepo, "distros/fedora-installer/migrate/prevent-set.list"), "w").write(
    "script plasma-session-start.sh\nservice dbus\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
sm._PREVENT_LIST_OVERRIDE = os.path.join(fakerepo, "distros/fedora-installer/migrate/prevent-set.list")
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

def fake_run(argv, **kw):
    return type("R", (), {"returncode": 1 if argv[:2] == ["rpm", "-q"] else 0, "stdout": ""})()

# dry-run writes nothing
rc = sm.main(["--dry-run"], run=fake_run)
check("dry-run exits 0", rc == 0)
check("dry-run wrote no services", not os.path.exists(os.path.join(root, "etc/schema-init/services")))

# full deploy
rc = sm.main([], run=fake_run)
check("deploy exits 0", rc == 0)
check("prevent-set landed", os.path.exists(os.path.join(root, "etc/schema-init/services/dbus.svc")))
check("boot entry created", os.path.exists(os.path.join(root, "boot/loader/entries/schema-init.conf")))
check("manifest saved", os.path.exists(os.path.join(root, sm.Manifest.PATH)))

# re-run guard
rc = sm.main([], run=fake_run)
check("re-migration is a no-op exit 0", rc == 0)

# uninstall
rc = sm.main(["--uninstall"], run=fake_run)
check("uninstall exits 0 + removes boot entry",
      rc == 0 and not os.path.exists(os.path.join(root, "boot/loader/entries/schema-init.conf")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
