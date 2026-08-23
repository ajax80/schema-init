#!/usr/bin/env python3
"""vt-mediation tests — VT mode injected via env, re-arm hook injected."""
import os, sys, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def load(mode):
    os.environ["SCHEMA_DOCTOR_VT_MODE"] = mode
    spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# VT_PROCESS → mediated → healthy
sd = load("process"); check("mediated passes", sd.VtMediation().detect() is None)

# VT_AUTO → not mediated → broken (the frozen-mouse path)
sd = load("auto"); c = sd.VtMediation()
f = c.detect(); check("auto flagged", f is not None)

# heal calls re-arm; if the hook flips the mode to process, verify passes
armed = {"n": 0}
def fake_rearm():
    armed["n"] += 1; os.environ["SCHEMA_DOCTOR_VT_MODE"] = "process"
c.rearm = fake_rearm
c.heal(f); check("heal calls re-arm", armed["n"] == 1 and c.verify() is True)

# hook unreachable → heal no-ops, verify stays False (degrades to report)
sd = load("auto"); c = sd.VtMediation(); c.rearm = lambda: None
c.heal(c.detect()); check("unreachable hook stays broken", c.verify() is False)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
