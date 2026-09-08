#!/usr/bin/env python3
"""panel-launcher-paths tests — fake plasmashell HOME + appletsrc, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
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

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
