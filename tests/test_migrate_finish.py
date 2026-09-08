#!/usr/bin/env python3
"""finish report tests."""
import os, json, tempfile, importlib.util

def test_finish_report():
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

    rep = sm.finish_report()
    assert "tailscaled" in rep and "docker" in rep, "names the leftover services"
    assert "GREEN" in rep, "shows the doctor result"
    assert "translate" in rep.lower(), "mentions the translate step"
    assert os.path.exists(os.path.join(root, "run/schema-init/migrate-finished")), "writes the once marker"
