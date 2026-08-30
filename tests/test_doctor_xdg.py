#!/usr/bin/env python3
"""xdg-data-dirs-dup tests — fake plasmashell env + HOME under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
home = os.path.join(root, "home", "u")
os.makedirs(home)


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

GUARD = os.path.join(home, ".config/plasma-workspace/env/zzzz-dedup-xdg-data-dirs.sh")
c = sd.XdgDataDirsDup()

# no plasmashell → cannot judge → clean
check("clean when plasmashell absent", c.detect() is None)

# plasmashell with a clean (no-dup) XDG_DATA_DIRS → clean
mkproc(10, "plasmashell", {"HOME": home, "XDG_DATA_DIRS": "/usr/share:/usr/local/share"})
check("clean when no duplicate", c.detect() is None)

# plasmashell with snapd added twice → the dup
dup = "/usr/local/share:/usr/share:/var/lib/snapd/desktop:/var/lib/snapd/desktop"
mkproc(10, "plasmashell", {"HOME": home, "XDG_DATA_DIRS": dup})
f = c.detect()
check("detect flags duplicate entry", f is not None)
check("detail names XDG_DATA_DIRS", f is not None and "XDG_DATA_DIRS" in f.detail)
check("detail names the doubled path", f is not None and "/var/lib/snapd/desktop" in f.detail)
check("finding is healable", f is not None and f.healable)

# heal writes the dedup guard; verify passes
snap = c.snapshot()
c.heal(f)
check("heal writes guard script", os.path.exists(GUARD))
check("verify true after heal", c.verify() is True)
check("guard is executable", os.access(GUARD, os.X_OK))

# with the guard present, detect goes quiet (live env still dup, but next login is clean)
check("clean once guard exists", c.detect() is None)

# back_out removes the guard we just created
c.back_out(snap)
check("back_out removes guard", not os.path.exists(GUARD))

# back_out leaves a pre-existing guard alone
os.makedirs(os.path.dirname(GUARD), exist_ok=True)
open(GUARD, "w").write("pre-existing\n")
snap2 = c.snapshot()
c.back_out(snap2)
check("back_out keeps a pre-existing guard", os.path.exists(GUARD))

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
