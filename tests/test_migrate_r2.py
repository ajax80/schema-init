import os, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_r2", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def _root():
    r = tempfile.mkdtemp()
    for d in ("var/lib", "etc/xdg/autostart"):
        os.makedirs(os.path.join(r, d))
    open(os.path.join(r, "etc/xdg/autostart/schema-wizard.desktop"), "w").close()
    return r

def _ok(*a):
    class R: returncode = 0; stdout = ""; stderr = ""
    return R()
def _fail(*a):
    class R: returncode = 1; stdout = ""; stderr = ""
    return R()

def test_arm_flip_requires_r1_heal():
    m = _load(); r = _root()
    m.stage.write_stage(m.stage.R1_HEAL, root=r)
    assert m.arm_flip(root=r, flip=lambda *a: _ok()) == 0
    assert m.stage.read_stage(r) == m.stage.R2_PENDING

def test_finish_r1_pending_advances_to_heal():
    m = _load(); r = _root()
    m.stage.write_stage(m.stage.R1_PENDING, root=r)
    assert m.advance_finish(root=r, flip=lambda *a: _ok()) == m.stage.R1_HEAL

def test_finish_r2_authoritative_is_done_and_tears_down():
    m = _load(); r = _root()
    m.stage.write_stage(m.stage.R2_PENDING, root=r)
    out = m.advance_finish(root=r, flip=lambda *a: _ok())     # is-authoritative rc 0
    assert out == m.stage.DONE
    assert not os.path.exists(os.path.join(r, "etc/xdg/autostart/schema-wizard.desktop"))

def test_finish_r2_not_authoritative_is_rolled_back():
    m = _load(); r = _root()
    m.stage.write_stage(m.stage.R2_PENDING, root=r)
    out = m.advance_finish(root=r, flip=lambda *a: _fail())   # is-authoritative rc 1
    assert out == m.stage.ROLLED_BACK
