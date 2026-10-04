#!/usr/bin/env python3
"""pstore-crash tests — fake /run marker under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root

spec = importlib.util.spec_from_file_location(
    "schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.PstoreCrash()
check("clean with no marker", c.detect() is None)

os.makedirs(os.path.join(root, "run/schema-init"))
marker = os.path.join(root, "run/schema-init/pstore-harvested")
open(marker, "w").write("\n")
check("empty marker is clean", c.detect() is None)

dest = "/var/lib/schema-init/pstore/20270115-080000"
open(marker, "w").write(dest + "\n")
f = c.detect()
check("harvest this boot is flagged", f is not None and not f.healable)
check("detail names the archive", f is not None and dest in f.detail)

print("PASS" if all(results) else "FAIL")
