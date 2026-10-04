#!/usr/bin/env python3
"""pstore-crash tests — fake /proc/stat + archive under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
os.makedirs(os.path.join(root, "proc"))
BTIME = 1_800_000_000
with open(os.path.join(root, "proc/stat"), "w") as fh:
    fh.write(f"cpu 1 2 3\nbtime {BTIME}\n")

spec = importlib.util.spec_from_file_location(
    "schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.PstoreCrash()
arch = os.path.join(root, "var/lib/schema-init/pstore")

check("clean with no archive", c.detect() is None)

old = os.path.join(arch, "20260101-000000")
os.makedirs(old)
os.utime(old, (BTIME - 86400, BTIME - 86400))
check("an archive from an earlier boot is not flagged", c.detect() is None)

new = os.path.join(arch, "20270115-080000")
os.makedirs(new)
os.utime(new, (BTIME + 5, BTIME + 5))
f = c.detect()
check("archive harvested this boot is flagged", f is not None and not f.healable)
check("detail names only the fresh archive", f is not None and new in f.detail and old not in f.detail)

print("PASS" if all(results) else "FAIL")
