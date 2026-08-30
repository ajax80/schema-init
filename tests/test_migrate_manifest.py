#!/usr/bin/env python3
"""manifest record/save/load tests."""
import os, sys, json, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

m = sm.Manifest()
m.add_file("/usr/bin/schema-init"); m.add_file("/usr/bin/schema-init")  # dedup
m.add_package("libavcodec-freeworld")
m.set_boot_entry("/boot/loader/entries/schema-init.conf")
m.save()

check("dedups files", m.files.count("/usr/bin/schema-init") == 1)
m2 = sm.Manifest.load()
check("reloads files", "/usr/bin/schema-init" in m2.files)
check("reloads packages", "libavcodec-freeworld" in m2.packages)
check("reloads boot entry", m2.boot_entry.endswith("schema-init.conf"))

# corrupt file → empty manifest, no crash
open(os.path.join(root, sm.Manifest.PATH), "w").write("{ not json")
m3 = sm.Manifest.load()
check("corrupt manifest loads empty", m3.files == [] and m3.boot_entry is None)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
