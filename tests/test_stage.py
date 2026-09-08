#!/usr/bin/env python3
"""stage machine tests — script-style, no root needed."""
import os, sys, json, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/stage.py")
spec = importlib.util.spec_from_file_location("stage", MOD)
stage = importlib.util.module_from_spec(spec); spec.loader.exec_module(stage)

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

def _root():
    r = tempfile.mkdtemp(); os.makedirs(os.path.join(r, "var/lib")); return r

# absent stage reads as INSTALLED
check("absent reads INSTALLED", stage.read_stage(_root()) == stage.INSTALLED)

# write then read roundtrip + 644 perms
r = _root()
stage.write_stage(stage.R1_PENDING, root=r)
check("roundtrip reads R1_PENDING", stage.read_stage(r) == stage.R1_PENDING)
check("stage file is 0644", oct(os.stat(os.path.join(r, stage.STAGE_PATH)).st_mode)[-3:] == "644")

# legal transition chain
r = _root()
stage.transition(stage.R1_PENDING, root=r)
stage.transition(stage.R1_HEAL, root=r)
check("legal transition to R1_HEAL", stage.read_stage(r) == stage.R1_HEAL)

# illegal transition raises ValueError (INSTALLED -> DONE not allowed)
r = _root()
raised = False
try:
    stage.transition(stage.DONE, root=r)
except ValueError:
    raised = True
check("illegal transition raises ValueError", raised)

# extra fields persist alongside ts
r = _root()
stage.write_stage(stage.R2_PENDING, root=r, extra={"snapshot": "@pre-schema"})
d = json.load(open(os.path.join(r, stage.STAGE_PATH)))
check("extra fields persist with ts", d["snapshot"] == "@pre-schema" and "ts" in d)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
