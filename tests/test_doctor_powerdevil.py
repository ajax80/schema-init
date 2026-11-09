#!/usr/bin/env python3
"""powerdevil-running tests — fake /proc + session under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
os.makedirs(os.path.join(root, "run/systemd/sessions"))
with open(os.path.join(root, "run/systemd/sessions", "1"), "w") as fh:
    fh.write("UID=%d\nVTNR=1\n" % os.getuid())


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

# The boot run fires once the logind session exists, seconds before
# plasma-session-start brings up plasmashell and PowerDevil together (#490).
check("clean before the desktop is up", c.detect() is None)
# the session scripts poll for it with pgrep while it is still starting
mkproc(900, "pgrep -x plasmashell")
check("a pgrep naming plasmashell is not the desktop", c.detect() is None)

mkproc(1000, "/usr/bin/plasmashell")
f = c.detect()
check("detect flags powerdevil down", f is not None and not f.healable)
check("detail names X-systemd-skip", f is not None and "X-systemd-skip" in f.detail)

mkproc(1100, "pgrep -f /usr/libexec/org_kde_powerdevil")
check("a pgrep naming powerdevil does not mask it", c.detect() is not None)

mkproc(1200, "/usr/libexec/org_kde_powerdevil")
check("clean when powerdevil runs", c.detect() is None)

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
