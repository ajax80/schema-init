import os, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_pb", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def test_prebuilt_skips_build_and_manifest():
    root = tempfile.mkdtemp()
    os.makedirs(os.path.join(root, "usr/bin"))
    open(os.path.join(root, "usr/bin/schema-init"), "w").close()
    os.environ["MIGRATE_ROOT"] = root
    m = _load()
    calls = []
    def fake_run(argv, *a, **k):
        calls.append(argv)
        class R: returncode = 0; stdout = ""
        return R()
    man = m.Manifest()
    m.provision_binaries(man, run=fake_run, prebuilt=True)
    assert not any("make" in " ".join(c) for c in calls)   # never built
    assert not any("schema-init" in f for f in man.files)   # RPM owns it
    del os.environ["MIGRATE_ROOT"]

def test_prebuilt_refuses_when_binary_absent():
    root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "usr/bin"))
    os.environ["MIGRATE_ROOT"] = root
    m = _load()
    try:
        raised = False
        try:
            m.provision_binaries(m.Manifest(), prebuilt=True)
        except RuntimeError:
            raised = True
        assert raised
    finally:
        del os.environ["MIGRATE_ROOT"]
