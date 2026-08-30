#!/usr/bin/env python3
"""uninstall reverses exactly the manifest — no live dnf (inject runner)."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
os.makedirs(os.path.join(root, "usr/bin")); os.makedirs(os.path.join(root, "boot/loader/entries"))
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

added = os.path.join(root, "usr/bin/schema-init"); open(added, "w").close()
untracked = os.path.join(root, "usr/bin/keepme"); open(untracked, "w").close()
entry = os.path.join(root, "boot/loader/entries/schema-init.conf"); open(entry, "w").close()

m = sm.Manifest(); m.add_file("/usr/bin/schema-init"); m.add_package("libavcodec-freeworld")
m.set_boot_entry("/boot/loader/entries/schema-init.conf"); m.save()

calls = []
def fake_run(argv, **kw):
    calls.append(argv); return type("R", (), {"returncode": 0, "stdout": ""})()

res = sm.uninstall(run=fake_run)
check("removed the tracked file", not os.path.exists(added))
check("left the untracked file", os.path.exists(untracked))
check("removed the boot entry", not os.path.exists(entry))
check("dnf remove called for the package",
      any("remove" in c and "libavcodec-freeworld" in c for c in calls))
check("manifest deleted", not os.path.exists(os.path.join(root, sm.Manifest.PATH)))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
