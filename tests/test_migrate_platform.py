#!/usr/bin/env python3
"""platform detection tests — fake os-release + KDE markers under MIGRATE_ROOT."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
os.makedirs(os.path.join(root, "etc")); os.makedirs(os.path.join(root, "usr/bin"))
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# no os-release → refuse
plat, why = sm.detect_platform()
check("refuse when os-release missing", plat is None and "fedora" in why.lower())

# fedora but no KDE marker → refuse
open(os.path.join(root, "etc/os-release"), "w").write('ID=fedora\nVERSION_ID=44\n')
plat, why = sm.detect_platform()
check("refuse fedora without KDE", plat is None and "kde" in why.lower())

# not fedora → refuse
open(os.path.join(root, "etc/os-release"), "w").write('ID=debian\n')
open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
plat, why = sm.detect_platform()
check("refuse non-fedora", plat is None)

# fedora + plasmashell → accept
open(os.path.join(root, "etc/os-release"), "w").write('ID=fedora\nVERSION_ID=44\n')
plat, why = sm.detect_platform()
check("accept fedora kde", plat == "fedora-kde")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
