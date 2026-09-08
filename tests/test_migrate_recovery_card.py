#!/usr/bin/env python3
"""recovery card writer tests — script-style."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_rc", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

m = _load()
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "home/jandoe")); os.makedirs(os.path.join(root, "boot"))
os.environ["MIGRATE_ROOT"] = root
try:
    paths = m.write_recovery_card({"user": "jandoe"}, root=root)
    home = os.path.join(root, "home/jandoe/schema-recovery.txt")
    boot = os.path.join(root, "boot/schema-recovery.txt")
    check("writes home + boot card", os.path.exists(home) and os.path.exists(boot))
    body = open(home).read()
    check("card body has schema-init + black", "(schema-init)" in body and "black" in body.lower())
    check("returns both paths", set(paths) == {"/home/jandoe/schema-recovery.txt", "/boot/schema-recovery.txt"})
finally:
    del os.environ["MIGRATE_ROOT"]

# home absent: write only the boot card, never create /home/<user>
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "boot"))
os.environ["MIGRATE_ROOT"] = root
try:
    paths = m.write_recovery_card({"user": "jandoe"}, root=root)
    check("skips home when it does not exist", not os.path.exists(os.path.join(root, "home")))
    check("still writes boot card", paths == ["/boot/schema-recovery.txt"])
finally:
    del os.environ["MIGRATE_ROOT"]

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
