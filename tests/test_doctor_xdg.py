#!/usr/bin/env python3
"""xdg-data-dirs-dup tests — fake plasmashell env + HOME under DOCTOR_ROOT, no root."""
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

# --- the desktop process is picked by owner and argv0, never by a word in its args
os.remove(GUARD)
rogue_home = os.path.join(root, "home", "rogue")
os.makedirs(rogue_home)
mkproc(10, "plasmashell", {"HOME": home, "XDG_DATA_DIRS": "/usr/share"})
mkproc(1, "python3 /tmp/x.py plasmashell", {"HOME": rogue_home, "XDG_DATA_DIRS": "/a:/a"})
check("word in another process's args is not the desktop", c.detect() is None)
check("real plasmashell found past the impostor", c._plasma()["pid"] == "10")

mkproc(1, "plasmashell", {"HOME": rogue_home, "XDG_DATA_DIRS": "/a:/a"})
os.remove(os.path.join(root, "proc", "10", "cmdline"))
set_session_uid(os.getuid() + 1)
check("plasmashell owned by a non-session uid is ignored", c.detect() is None)
set_session_uid(os.getuid())

# --- a link planted at the guard path must not redirect the root write
g2 = os.path.join(rogue_home, sd.DEDUP_GUARD_REL)
os.makedirs(os.path.dirname(g2))
outside = os.path.join(root, "created-outside-home")
os.symlink(outside, g2)
f = c.detect()
check("planted link still reads as missing guard", f is not None)
c.heal(f)
check("heal does not write through a planted link", not os.path.exists(outside))
check("verify reports the failed heal", c.verify() is False)

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
