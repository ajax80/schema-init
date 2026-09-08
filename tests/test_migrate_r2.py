#!/usr/bin/env python3
"""arm-flip / advance-finish (R2) tests — script-style."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_r2", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

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

# arm_flip requires R1_HEAL and advances to R2_PENDING
m = _load(); r = _root()
m.stage.write_stage(m.stage.R1_HEAL, root=r)
check("arm_flip returns 0 from R1_HEAL", m.arm_flip(root=r, flip=lambda *a: _ok()) == 0)
check("arm_flip advances to R2_PENDING", m.stage.read_stage(r) == m.stage.R2_PENDING)

# finish from R1_PENDING advances to R1_HEAL
m = _load(); r = _root()
m.stage.write_stage(m.stage.R1_PENDING, root=r)
check("finish R1_PENDING -> R1_HEAL", m.advance_finish(root=r, flip=lambda *a: _ok()) == m.stage.R1_HEAL)

# finish from R2_PENDING, authoritative -> DONE + teardown
m = _load(); r = _root()
m.stage.write_stage(m.stage.R2_PENDING, root=r)
out = m.advance_finish(root=r, flip=lambda *a: _ok())     # is-authoritative rc 0
check("finish R2 authoritative -> DONE", out == m.stage.DONE)
check("finish R2 tears down autostart", not os.path.exists(os.path.join(r, "etc/xdg/autostart/schema-wizard.desktop")))

# finish from R2_PENDING, not authoritative -> ROLLED_BACK
m = _load(); r = _root()
m.stage.write_stage(m.stage.R2_PENDING, root=r)
out = m.advance_finish(root=r, flip=lambda *a: _fail())   # is-authoritative rc 1
check("finish R2 not-authoritative -> ROLLED_BACK", out == m.stage.ROLLED_BACK)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
