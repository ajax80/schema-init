#!/usr/bin/env python3
"""prebuilt deploy mode tests — script-style."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_pb", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

# prebuilt skips build + does not claim schema-init in the manifest (RPM owns it)
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "usr/bin"))
for b in ("schema-init", "schema-ctl", "schema-subreaper"):
    open(os.path.join(root, "usr/bin", b), "w").close()
os.environ["MIGRATE_ROOT"] = root
m = _load()
calls = []
def fake_run(argv, *a, **k):
    calls.append(argv)
    class R: returncode = 0; stdout = ""
    return R()
man = m.Manifest()
m.provision_binaries(man, run=fake_run, prebuilt=True)
check("prebuilt never builds", not any("make" in " ".join(c) for c in calls))
check("manifest omits schema-init", not any("schema-init" in f for f in man.files))
del os.environ["MIGRATE_ROOT"]

# prebuilt refuses when a required binary is absent
root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "usr/bin"))
os.environ["MIGRATE_ROOT"] = root
m = _load()
raised = False
try:
    m.provision_binaries(m.Manifest(), prebuilt=True)
except RuntimeError:
    raised = True
del os.environ["MIGRATE_ROOT"]
check("prebuilt refuses when binary absent", raised)

# prebuilt refuses when only schema-init is present (must have all of PREBUILT_BINS)
root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "usr/bin"))
open(os.path.join(root, "usr/bin/schema-init"), "w").close()
os.environ["MIGRATE_ROOT"] = root
m = _load()
raised = False
try:
    m.provision_binaries(m.Manifest(), prebuilt=True)
except RuntimeError:
    raised = True
del os.environ["MIGRATE_ROOT"]
check("prebuilt refuses partial install", raised)

# --dry-run --prebuilt prints the plan, never raises, even with no binaries present
root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "usr/bin"))
os.environ["MIGRATE_ROOT"] = root
m = _load()
raised = False
try:
    m.provision_binaries(m.Manifest(), dry_run=True, prebuilt=True)
except RuntimeError:
    raised = True
del os.environ["MIGRATE_ROOT"]
check("dry-run+prebuilt does not raise", not raised)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
