#!/usr/bin/env python3
"""ksycoca-loop tests — fake plasmashell/kded6 env under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root


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

c = sd.KsycocaLoop()

# only one of the pair present → cannot judge → clean
mkproc(1, "plasmashell", {"XDG_MENU_PREFIX": ""})
check("clean when kded6 absent", c.detect() is None)

# mismatch: plasmashell unset, kded6 plasma- → the loop
mkproc(2, "kded6", {"XDG_MENU_PREFIX": "plasma-"})
f = c.detect()
check("detect flags prefix mismatch", f is not None)
check("detail names XDG_MENU_PREFIX", f is not None and "XDG_MENU_PREFIX" in f.detail)

# both agree → clean
mkproc(1, "plasmashell", {"XDG_MENU_PREFIX": "plasma-"})
check("clean when prefixes agree", c.detect() is None)

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
