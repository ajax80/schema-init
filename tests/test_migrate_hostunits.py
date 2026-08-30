#!/usr/bin/env python3
"""host unit generation tests."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

profile = {"mounts": [
    {"src": "UUID=aaa", "target": "/", "fstype": "ext4", "opts": "defaults"},
    {"src": "UUID=bbb", "target": "/home", "fstype": "xfs", "opts": "defaults"},
    {"src": "UUID=ccc", "target": "/mnt/data", "fstype": "ext4", "opts": "defaults"}]}
m = sm.Manifest()
dst = sm.generate_host_units(profile, m)
home_unit = os.path.join(root, "etc/schema-init/services/mount-home.svc")
data_unit = os.path.join(root, "etc/schema-init/services/mount-mnt-data.svc")
root_unit = os.path.join(root, "etc/schema-init/services/mount-root.svc")
check("home mount unit written", os.path.exists(home_unit))
check("nested mount slug is path-safe", os.path.exists(data_unit))
check("no unit generated for /", not os.path.exists(root_unit))
check("manifest does not record /", not any("mount-root.svc" in f for f in m.files))
body = open(home_unit).read()
check("unit mounts the right target", "/home" in body and "UUID=bbb" in body)
check("unit is oneshot needs_root", "oneshot=1" in body and "needs_root=1" in body)
check("manifest recorded units", any("mount-home.svc" in f for f in m.files))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
