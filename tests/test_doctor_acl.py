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
dbdir = os.path.join(root, "run/udev/data"); os.makedirs(dbdir)
with open(os.path.join(dbdir, "c226:0"), "w") as fh:
    fh.write("N:dri/card0\nG:uaccess\nQ:uaccess\nQ:seat\n")
os.makedirs(os.path.join(root, "dev/input"))
# keyboards/mice and render nodes carry no uaccess tag: systemd never grants
# them to the user (render is 0666; input goes through logind TakeDevice)
event = os.path.join(root, "dev/input/event0"); open(event, "w").close()
render = os.path.join(root, "dev/dri/renderD128"); open(render, "w").close()
with open(os.path.join(dbdir, "c13:64"), "w") as fh:
    fh.write("N:input/event0\nQ:seat\n")

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

# uaccess-tagged usb/hidraw nodes (SDR/FIDO) are discovered from the udev db,
# non-uaccess devices are not
os.makedirs(os.path.join(root, "dev/extra"))
sdr = os.path.join(root, "dev/extra/sdr0"); open(sdr, "w").close()
nope = os.path.join(root, "dev/extra/nope"); open(nope, "w").close()
with open(os.path.join(dbdir, "c500:0"), "w") as fh:
    fh.write("N:extra/sdr0\nG:uaccess\nQ:uaccess\nQ:seat\n")
with open(os.path.join(dbdir, "c501:0"), "w") as fh:
    fh.write("N:extra/nope\nQ:seat\n")
nodes = c._nodes()
check("uaccess db node discovered", sdr in nodes)
check("non-uaccess db node skipped", nope not in nodes)
check("untagged input node not checked", event not in nodes)
check("untagged render node not checked", render not in nodes)
f = c.detect()
check("untagged nodes without user ACL are not flagged",
      f is None or ("event0" not in f.detail and "renderD128" not in f.detail))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
