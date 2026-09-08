#!/usr/bin/env python3
"""flip seatbelt rail-wiring tests — script-style."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_sb", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

# deploy installs the seatbelt .svc pointing at the packaged healthcheck, oneshot
root = tempfile.mkdtemp()
os.environ["MIGRATE_ROOT"] = root
m = _load()
try:
    man = m.Manifest()
    m.install_flip_seatbelt(man)
    svc = os.path.join(root, "etc/schema-init/services/schema-udev-healthcheck.svc")
    check("writes the seatbelt .svc", os.path.exists(svc))
    body = open(svc).read()
    check("exec is the packaged healthcheck", m.SEATBELT_HELPER in body)
    check("runs as a oneshot", "oneshot=1" in body)
    check("recorded in the manifest", "/etc/schema-init/services/schema-udev-healthcheck.svc" in man.files)
finally:
    del os.environ["MIGRATE_ROOT"]

# dry-run writes nothing, records nothing
root = tempfile.mkdtemp()
os.environ["MIGRATE_ROOT"] = root
m = _load()
try:
    man = m.Manifest()
    m.install_flip_seatbelt(man, dry_run=True)
    svc = os.path.join(root, "etc/schema-init/services/schema-udev-healthcheck.svc")
    check("dry-run writes no .svc", not os.path.exists(svc))
    check("dry-run records nothing", man.files == [])
finally:
    del os.environ["MIGRATE_ROOT"]

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
