#!/usr/bin/env python3
"""deploy imports systemd-enabled units via the real schema-import; uninstall removes them."""
import os, sys, json, tempfile, subprocess, importlib.util

results = []
def check(n, ok): results.append(bool(ok)); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
for d in ("usr/lib/systemd/system", "etc/systemd/system/multi-user.target.wants",
          "etc/schema-init/services", "usr/bin", "var/lib/schema-init", "run/schema-init"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "usr/bin/smartd"), "w").close()
os.chmod(os.path.join(root, "usr/bin/smartd"), 0o755)
open(os.path.join(root, "usr/lib/systemd/system/smartd.service"), "w").write(
    "[Unit]\nDescription=disk health\n[Service]\nExecStart=/usr/bin/smartd -n\n"
    "[Install]\nWantedBy=multi-user.target\n")
os.symlink("/usr/lib/systemd/system/smartd.service",
           os.path.join(root, "etc/systemd/system/multi-user.target.wants/smartd.service"))
open(os.path.join(root, "etc/schema-init/services/dbus.svc"), "w").write("name=dbus\n")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)

m = sm.Manifest()
check("dry-run imports nothing", sm.import_enabled_units(m, dry_run=True) == []
      and not os.path.exists(os.path.join(root, "etc/schema-init/services/smartd.svc")))

added = sm.import_enabled_units(m, run=subprocess.run)
check("enabled smartd imported", os.path.exists(os.path.join(root, "etc/schema-init/services/smartd.svc")))
check("manifest records the import", "/etc/schema-init/services/smartd.svc" in m.files)
check("pre-existing svc not claimed", "/etc/schema-init/services/dbus.svc" not in m.files)
check("import log written", os.path.exists(os.path.join(root, "var/log/schema-init/migrate-import.log")))

open(os.path.join(root, "var/lib/schema-init/migrate-profile.json"), "w").write(
    json.dumps({"services": {"leftover": ["smartd", "tailscaled"]}}))
rep = sm.finish_report()
check("imported unit not reported as leftover", "smartd" not in rep and "tailscaled" in rep)

m.save()
sm.uninstall(run=lambda *a, **k: type("R", (), {"returncode": 0, "stdout": ""})())
check("uninstall removes the import", not os.path.exists(os.path.join(root, "etc/schema-init/services/smartd.svc")))
check("uninstall keeps pre-existing svc", os.path.exists(os.path.join(root, "etc/schema-init/services/dbus.svc")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
