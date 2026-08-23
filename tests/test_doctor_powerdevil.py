#!/usr/bin/env python3
"""powerdevil-running tests — fake /proc + session under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
os.makedirs(os.path.join(root, "run/systemd/sessions"))
with open(os.path.join(root, "run/systemd/sessions", "1"), "w") as fh:
    fh.write("UID=1000\nVTNR=1\n")


def mkproc(pid, cmd, env=None):
    d = os.path.join(root, "proc", str(pid))
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "cmdline"), "wb") as fh:
        fh.write(cmd.encode() + b"\0")
    with open(os.path.join(d, "environ"), "wb") as fh:
        fh.write(b"".join((k + "=" + v).encode() + b"\0" for k, v in (env or {}).items()))


spec = importlib.util.spec_from_file_location(
    "schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.PowerDevilRunning()

mkproc(1000, "plasmashell")
f = c.detect()
check("detect flags powerdevil down", f is not None and not f.healable)
check("detail names X-systemd-skip", f is not None and "X-systemd-skip" in f.detail)

mkproc(1200, "/usr/libexec/org_kde_powerdevil")
check("clean when powerdevil runs", c.detect() is None)

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
