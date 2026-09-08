import os, sys, tempfile, importlib.util, pytest
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/stage.py")
spec = importlib.util.spec_from_file_location("stage", MOD)
stage = importlib.util.module_from_spec(spec); spec.loader.exec_module(stage)

def _root():
    r = tempfile.mkdtemp(); os.makedirs(os.path.join(r, "var/lib")); return r

def test_absent_reads_installed():
    assert stage.read_stage(_root()) == stage.INSTALLED

def test_write_then_read_roundtrip():
    r = _root()
    stage.write_stage(stage.R1_PENDING, root=r)
    assert stage.read_stage(r) == stage.R1_PENDING
    assert oct(os.stat(os.path.join(r, stage.STAGE_PATH)).st_mode)[-3:] == "644"

def test_legal_transition():
    r = _root()
    stage.transition(stage.R1_PENDING, root=r)
    stage.transition(stage.R1_HEAL, root=r)
    assert stage.read_stage(r) == stage.R1_HEAL

def test_illegal_transition_raises():
    r = _root()
    with pytest.raises(ValueError):
        stage.transition(stage.DONE, root=r)   # INSTALLED -> DONE is not allowed

def test_extra_fields_persist():
    r = _root()
    stage.write_stage(stage.R2_PENDING, root=r, extra={"snapshot": "@pre-schema"})
    import json
    d = json.load(open(os.path.join(r, stage.STAGE_PATH)))
    assert d["snapshot"] == "@pre-schema" and "ts" in d
