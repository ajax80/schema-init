#!/usr/bin/env python3
"""session-agents tests — fake session + KDE procs under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
os.makedirs(os.path.join(root, "run/systemd/sessions"))
with open(os.path.join(root, "run/systemd/sessions", "1"), "w") as fh:
    fh.write("UID=1000\nVTNR=1\n")


def mkproc(pid, cmd):
    d = os.path.join(root, "proc", str(pid))
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "cmdline"), "wb") as fh:
        fh.write(cmd.encode() + b"\0")
    open(os.path.join(d, "environ"), "wb").close()


def rmproc(pid):
    import shutil
    shutil.rmtree(os.path.join(root, "proc", str(pid)), ignore_errors=True)


spec = importlib.util.spec_from_file_location(
    "schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.SessionAgents()

# no plasmashell → not a live KDE desktop → clean (a GNOME stranger won't false-flag)
check("clean when no plasmashell", c.detect() is None)

# KDE session with both agents up → clean
mkproc(10, "/usr/bin/plasmashell")
mkproc(11, "/usr/libexec/kglobalacceld")
mkproc(12, "/usr/libexec/kf6/polkit-kde-authentication-agent-1")
check("clean when both agents run", c.detect() is None)

# kglobalacceld missing → flagged, names it, deferred/not healable
rmproc(11)
f = c.detect()
check("detect flags missing shortcut daemon", f is not None)
check("detail names kglobalacceld", f is not None and "kglobalacceld" in f.detail)
check("finding not healable", f is not None and f.healable is False)

# both agents missing → names both
rmproc(12)
f = c.detect()
check("detail names polkit agent too", f is not None and "polkit" in f.detail.lower())

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
