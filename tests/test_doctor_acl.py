#!/usr/bin/env python3
"""card-input-acl tests — real setfacl/getfacl on temp files, no root needed."""
import os, sys, tempfile, subprocess, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
# fake a session so active_uid() resolves to *this* user
os.makedirs(os.path.join(root, "run/systemd/sessions"))
uid = os.getuid()
with open(os.path.join(root, "run/systemd/sessions", "1"), "w") as fh:
    fh.write(f"UID={uid}\nVTNR=1\n")
os.makedirs(os.path.join(root, "dev/dri"))
node = os.path.join(root, "dev/dri/card0")
open(node, "w").close()   # stand-in device node — ACLs apply to any file

spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.CardInputAcl()
# no user ACL yet → detect finds it broken
subprocess.run(["setfacl", "-b", node], check=True)
f = c.detect(); check("detect flags missing ACL", f is not None)

# heal → user gets rw, verify passes
snap = c.snapshot(); c.heal(f); check("heal applies rw", c.verify() is True)
out = subprocess.run(["getfacl", "-pn", node], capture_output=True, text=True).stdout
check("getfacl shows user rw", f"user:{uid}:rw" in out)

# back_out restores the pre-heal ACL (no user entry)
c.back_out(snap)
out = subprocess.run(["getfacl", "-pn", node], capture_output=True, text=True).stdout
check("back_out removes user rw", f"user:{uid}:rw" not in out)

# idempotent: heal twice, second is a no-op that still verifies
f = c.detect(); c.heal(f); c.heal(c.detect() or sd.Finding("x")); check("idempotent", c.verify() is True)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
