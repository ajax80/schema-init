#!/usr/bin/env python3
"""finish report tests."""
import os, sys, json, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
for d in ("var/lib/schema-init", "run/schema-init"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "var/lib/schema-init/migrate-profile.json"), "w").write(
    json.dumps({"platform": "fedora-kde", "services": {"leftover": ["tailscaled", "docker"]}}))
open(os.path.join(root, "run/schema-init/doctor-status"), "w").write("schema-doctor: GREEN\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

rep = sm.finish_report()
check("names the leftover services", "tailscaled" in rep and "docker" in rep)
check("shows the doctor result", "GREEN" in rep)
check("mentions the translate step", "translate" in rep.lower())
check("writes the once marker", os.path.exists(os.path.join(root, "run/schema-init/migrate-finished")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
