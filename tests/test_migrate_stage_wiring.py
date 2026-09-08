import os, io, tempfile, importlib.util, contextlib
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_sw", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def _fedora_kde_root():
    root = tempfile.mkdtemp()
    for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib", "home/jandoe"):
        os.makedirs(os.path.join(root, d))
    open(os.path.join(root, "etc/os-release"), "w").write("ID=fedora\n")
    open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
    open(os.path.join(root, "usr/bin/schema-init"), "w").close()  # prebuilt present
    open(os.path.join(root, "etc/fstab"), "w").write("UUID=aaa / ext4 defaults 0 1\n")
    open(os.path.join(root, "etc/passwd"), "w").write("jandoe:x:1000:1000::/home/jandoe:/bin/bash\n")
    open(os.path.join(root, "boot/loader/entries/f.conf"), "w").write(
        "title Fedora\nversion 6.10.0\noptions root=UUID=aaa ro\n")
    return root

def test_deploy_sets_r1_pending_and_writes_card():
    root = _fedora_kde_root()
    os.environ["MIGRATE_ROOT"] = root; os.environ["MIGRATE_KERNEL"] = "6.10.0"
    m = _load()
    try:
        rc = m.main(["--deploy", "--prebuilt"], run=lambda *a, **k: type("R", (), {"returncode":0,"stdout":""})())
        assert rc == 0
        assert m.stage.read_stage(root) == m.stage.R1_PENDING
        assert os.path.exists(os.path.join(root, "home/jandoe/schema-recovery.txt"))
    finally:
        del os.environ["MIGRATE_ROOT"]; del os.environ["MIGRATE_KERNEL"]

def test_stage_verb_prints_current():
    root = _fedora_kde_root(); os.environ["MIGRATE_ROOT"] = root
    m = _load()
    try:
        m.stage.write_stage(m.stage.R1_HEAL, root=root)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            m.main(["--stage"])
        assert "R1_HEAL" in buf.getvalue()
    finally:
        del os.environ["MIGRATE_ROOT"]
