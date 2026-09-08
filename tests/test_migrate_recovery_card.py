import os, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_rc", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def test_writes_home_and_boot():
    m = _load()
    root = tempfile.mkdtemp()
    os.makedirs(os.path.join(root, "home/jandoe")); os.makedirs(os.path.join(root, "boot"))
    os.environ["MIGRATE_ROOT"] = root
    try:
        paths = m.write_recovery_card({"user": "jandoe"}, root=root)
        home = os.path.join(root, "home/jandoe/schema-recovery.txt")
        boot = os.path.join(root, "boot/schema-recovery.txt")
        assert os.path.exists(home) and os.path.exists(boot)
        body = open(home).read()
        assert "(schema-init)" in body and "black" in body.lower()
        assert set(paths) == {"/home/jandoe/schema-recovery.txt", "/boot/schema-recovery.txt"}
    finally:
        del os.environ["MIGRATE_ROOT"]
