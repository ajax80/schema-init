#!/usr/bin/env python3
"""session-single tests — orphan #31 detection against a fake session tree."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def fresh(sessions, active_vt="tty1"):
    root = tempfile.mkdtemp()
    sd_dir = os.path.join(root, "run/systemd/sessions"); os.makedirs(sd_dir)
    tty = os.path.join(root, "sys/class/tty/tty0"); os.makedirs(tty)
    with open(os.path.join(tty, "active"), "w") as fh: fh.write(active_vt + "\n")
    for sid, body in sessions.items():
        with open(os.path.join(sd_dir, sid), "w") as fh: fh.write(body)
    os.environ["DOCTOR_ROOT"] = root
    spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# healthy: real session 1 on VTNR 1, no orphan
sd = fresh({"1": "UID=1000\nVTNR=1\nLEADER=1106\n"})
check("clean session passes", sd.SessionSingle().detect() is None)

# broken: only the synthesised legacy #31 on VTNR 0
sd = fresh({"31": "LEGACY_ID=31\nUID=1000\nVTNR=0\nLEADER=\n"})
f = sd.SessionSingle().detect()
check("orphan #31 flagged", f is not None and "31" in f.detail)

# broken: #31 coexists with a real session but active VT has no matching session
sd = fresh({"31": "UID=1000\nVTNR=0\n", "1": "UID=1000\nVTNR=1\n"}, active_vt="tty2")
check("active VT unbacked flagged", sd.SessionSingle().detect() is not None)

# grade is DEFERRED (never auto-heals)
check("grade DEFERRED", sd.SessionSingle().grade == sd.DEFERRED)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
