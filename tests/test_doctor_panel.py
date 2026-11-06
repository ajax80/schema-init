#!/usr/bin/env python3
"""panel-launcher-paths tests — fake plasmashell HOME + appletsrc, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
os.makedirs(os.path.join(root, "run/systemd/sessions"))
def set_session_uid(uid):
    with open(os.path.join(root, "run/systemd/sessions/1"), "w") as fh:
        fh.write("UID=%d\nACTIVE=1\n" % uid)
set_session_uid(os.getuid())
home = os.path.join(root, "home", "u")
os.makedirs(os.path.join(home, ".config"))
APPLETS = os.path.join(home, ".config/plasma-org.kde.plasma.desktop-appletsrc")


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

c = sd.PanelLauncherPaths()

# no plasmashell → clean
check("clean when no plasmashell", c.detect() is None)

mkproc(10, "plasmashell", {"HOME": home})

# no appletsrc yet → clean
check("clean when appletsrc absent", c.detect() is None)

# pins all in app-id form → clean
open(APPLETS, "w").write(
    "[Containments][2][Applets][5][Configuration][General]\n"
    "launchers=applications:org.kde.dolphin.desktop,applications:firefox.desktop\n")
check("clean when pins are app-id form", c.detect() is None)

# a fragile file:// pin on a drive path → flagged
open(APPLETS, "w").write(
    "[Containments][2][Applets][5][Configuration][General]\n"
    "launchers=applications:firefox.desktop,"
    "file:///mnt/XtraSpace/flatpak/exports/share/applications/org.mozilla.thunderbird.desktop\n")
f = c.detect()
check("detect flags file:// pin", f is not None)
check("detail names the fragile path scheme", f is not None and "file://" in f.detail)
check("finding not healable", f is not None and f.healable is False)

# --- root reads this path: not through a link, and never blocking on a FIFO
import signal; signal.alarm(20)  # a blocked open must fail the test, not hang it
secret = os.path.join(root, "root-only")
with open(secret, "w") as fh:
    fh.write("launchers=file:///secret/from-root-file.desktop\n")
if os.path.lexists(APPLETS):
    os.remove(APPLETS)
os.symlink(secret, APPLETS)
check("does not read through a planted link", c.detect() is None)
os.remove(APPLETS)
os.mkfifo(APPLETS)
check("returns with a FIFO at the applets path", c.detect() is None)
os.remove(APPLETS)

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
