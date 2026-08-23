#!/usr/bin/env python3
"""login1-power tests — probe injected, no live bus touched."""
import os, sys, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.Login1Power()

# all methods answer → healthy
c.probe = lambda m: (0, 's "yes"')
check("all-answered passes", c.detect() is None)

# one method errors → flagged, and names the failing method
def one_fails(m): return (1, "Unknown method") if m == "ListInhibitors" else (0, 's "yes"')
c.probe = one_fails
f = c.detect()
check("failing method flagged", f is not None and "ListInhibitors" in f.detail)

# grade DEFERRED, not auto-healed
check("grade DEFERRED", c.grade == sd.DEFERRED)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
