#!/usr/bin/env python3
"""session-agents tests — fake session + KDE procs under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
os.makedirs(os.path.join(root, "run/systemd/sessions"))
with open(os.path.join(root, "run/systemd/sessions", "1"), "w") as fh:
    fh.write("UID=%d\nVTNR=1\n" % os.getuid())


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
# a command line naming them is not them: earlyoom's --avoid list names
# plasmashell and kwin_wayland on every box, a session script's pgrep polls
mkproc(5, "/usr/bin/earlyoom --avoid ^(kwin_wayland|plasmashell|kglobalacceld)$")
mkproc(6, "pgrep -f polkit-kde-authentication-agent-1")
check("a command line naming plasmashell is not the desktop", c.detect() is None)

# KDE session with both agents up → clean
mkproc(10, "/usr/bin/plasmashell")
mkproc(11, "/usr/libexec/kglobalacceld")
mkproc(12, "/usr/libexec/kf6/polkit-kde-authentication-agent-1")
check("clean when both agents run", c.detect() is None)

# kglobalacceld missing → flagged, names it, deferred/not healable
rmproc(11)
f = c.detect()
check("detect flags missing shortcut daemon despite earlyoom naming kwin_wayland", f is not None)
check("detail names kglobalacceld", f is not None and "kglobalacceld" in f.detail)
check("finding not healable", f is not None and f.healable is False)

# Plasma 6 Wayland: kwin_wayland hosts kglobalaccel in-process → clean
mkproc(13, "/usr/bin/kwin_wayland --wayland-fd 7 --socket wayland-0")
check("clean when kwin_wayland hosts shortcuts", c.detect() is None)
rmproc(13)

# both agents missing → names both
rmproc(12)
f = c.detect()
check("detail names polkit agent too, despite a pgrep naming it", f is not None and "polkit" in f.detail.lower())

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
