#!/usr/bin/env python3
"""hardening-annotation / --hardening-lint tests for schema-doctor."""
import os, sys, io, tempfile, contextlib, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def load(root):
    os.environ["DOCTOR_ROOT"] = root
    spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}")

FULL = "no_new_privs=1\nprivate_tmp=1\nprotect_system=1\nprotect_home=0\n"
root = tempfile.mkdtemp()
svcd = os.path.join(root, "etc/schema-init/services")
os.makedirs(svcd)
def svc(name, body): open(os.path.join(svcd, name + ".svc"), "w").write("name=%s\nexec=/bin/x\n%s" % (name, body))
def switch(txt):
    p = os.path.join(root, "etc/schema-init/hardening-default")
    if txt is None:
        if os.path.exists(p): os.remove(p)
    else:
        open(p, "w").write(txt)
def lint():
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = sd.main(["--hardening-lint"])
    return rc, buf.getvalue()

sd = load(root)
chk = next(c for c in sd.REGISTRY if c.name == "hardening-annotation")

svc("done", FULL)
svc("zeroes", "no_new_privs=0\nprivate_tmp=0\nprotect_system=0\nprotect_home=0\n")
check("fully annotated host: lint clean", sd.hardening_unannotated() == [])
rc, out = lint()
check("fully annotated host: exit 0", rc == 0 and "0 .svc not fully annotated" in out)

svc("bare", "")
svc("partial", "protect_system=0\n")
svc("commented", "# no_new_privs=1\n" + FULL.replace("no_new_privs=1\n", ""))
bad = dict(sd.hardening_unannotated())
check("bare lists all four", bad.get("bare.svc") == list(sd.HARDENING_KEYS))
check("partial lists the three it left out",
      bad.get("partial.svc") == ["no_new_privs", "private_tmp", "protect_home"])
check("a commented key does not count", bad.get("commented.svc") == ["no_new_privs"])
check("annotated svcs not listed", "done.svc" not in bad and "zeroes.svc" not in bad)
rc, out = lint()
check("lint exit 1 and names each", rc == 1 and "bare.svc: no_new_privs private_tmp protect_system protect_home" in out
      and "3 .svc not fully annotated" in out)

switch(None)
check("switch absent: check clean", chk.detect() is None)
switch("ON\n")
check("switch 'ON': check clean", chk.detect() is None)
switch("  on \n")
f = chk.detect()
check("switch on: check reports", f is not None and "3 .svc" in f.detail and "bare.svc" in f.detail)
check("not healable", f is not None and f.healable is False and chk.grade == sd.DEFERRED)
switch(None)
rc, _ = lint()
check("lint ignores the switch", rc == 1)

# A knob set in a drop-in counts; files PID 1 would skip do not.
os.makedirs(os.path.join(svcd, "partial.svc.d"))
open(os.path.join(svcd, "partial.svc.d/10-h.conf"), "w").write("no_new_privs=1\nprivate_tmp=1\n")
open(os.path.join(svcd, "partial.svc.d/.#20-h.conf"), "w").write("protect_home=1\n")
open(os.path.join(svcd, "partial.svc.d/30-h.txt"), "w").write("protect_home=1\n")
check("drop-in knobs count, skipped files do not",
      dict(sd.hardening_unannotated()).get("partial.svc") == ["protect_home"])
for d, f in (("m@.svc.d", "50.conf"), ("m@a.svc.d", "10.conf")):
    os.makedirs(os.path.join(svcd, d))
    open(os.path.join(svcd, d, f), "w").write("")
check("svc_files order: base, then template dir, then instance dir",
      [os.path.relpath(p, svcd) for p in sd.svc_files(os.path.join(svcd, "m@a.svc"))]
      == ["m@a.svc", "m@.svc.d/50.conf", "m@a.svc.d/10.conf"])

ochk = next(c for c in sd.REGISTRY if c.name == "orphan-dropins")
svc("m@a", FULL)
check("used drop-in dirs are not orphans", ochk.detect() is None)
os.makedirs(os.path.join(svcd, "ghost.svc.d"))
os.makedirs(os.path.join(svcd, "t@.svc.d"))
f = ochk.detect()
check("orphan dirs reported", f is not None and "ghost.svc.d" in f.detail and "t@.svc.d" in f.detail
      and "partial.svc.d" not in f.detail and "m@.svc.d" not in f.detail and f.healable is False)

# Every shipped .svc in the repo is fully annotated: lint each source dir as
# if it were a host's services dir.
import glob, shutil
dirs = sorted({os.path.dirname(f) for f in glob.glob(os.path.join(REPO, "**/*.svc"), recursive=True)
               if "/tests/" not in f})
for d in dirs:
    r = tempfile.mkdtemp(); sdir = os.path.join(r, "etc/schema-init/services"); os.makedirs(sdir)
    for f in glob.glob(os.path.join(d, "*.svc")): shutil.copy(f, sdir)
    bad = load(r).hardening_unannotated()
    check("repo %s fully annotated%s" % (os.path.relpath(d, REPO), "" if not bad else " " + str(bad)), not bad)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
