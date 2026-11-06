#!/usr/bin/env python3
"""recovery card writer tests — script-style."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_rc", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

m = _load()
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "home/jandoe")); os.makedirs(os.path.join(root, "boot"))
os.environ["MIGRATE_ROOT"] = root
try:
    paths = m.write_recovery_card({"user": "jandoe", "uid": os.getuid()}, root=root)
    home = os.path.join(root, "home/jandoe/schema-recovery.txt")
    boot = os.path.join(root, "boot/schema-recovery.txt")
    check("writes home + boot card", os.path.exists(home) and os.path.exists(boot))
    body = open(home).read()
    check("card body has schema-init + black", "(schema-init)" in body and "black" in body.lower())
    check("returns both paths", set(paths) == {"/home/jandoe/schema-recovery.txt", "/boot/schema-recovery.txt"})
finally:
    del os.environ["MIGRATE_ROOT"]

# home absent: write only the boot card, never create /home/<user>
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "boot"))
os.environ["MIGRATE_ROOT"] = root
try:
    paths = m.write_recovery_card({"user": "jandoe"}, root=root)
    check("skips home when it does not exist", not os.path.exists(os.path.join(root, "home")))
    check("still writes boot card", paths == ["/boot/schema-recovery.txt"])
finally:
    del os.environ["MIGRATE_ROOT"]

# a link planted at the card path must not redirect a root write and chown
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "home/jandoe")); os.makedirs(os.path.join(root, "boot"))
outside = os.path.join(root, "outside")
open(outside, "w").write("ORIGINAL\n"); os.chmod(outside, 0o600)
os.symlink(outside, os.path.join(root, "home/jandoe/schema-recovery.txt"))
dangling = os.path.join(root, "would-be-created")
os.environ["MIGRATE_ROOT"] = root
try:
    paths = m.write_recovery_card({"user": "jandoe", "uid": os.getuid()}, root=root)
    check("planted link: target content untouched", open(outside).read() == "ORIGINAL\n")
    check("planted link: target mode untouched", os.stat(outside).st_mode & 0o777 == 0o600)
    check("planted link: not reported as written", paths == ["/boot/schema-recovery.txt"])
    os.remove(os.path.join(root, "home/jandoe/schema-recovery.txt"))
    os.symlink(dangling, os.path.join(root, "home/jandoe/schema-recovery.txt"))
    m.write_recovery_card({"user": "jandoe", "uid": os.getuid()}, root=root)
    check("dangling link: nothing created", not os.path.exists(dangling))
finally:
    del os.environ["MIGRATE_ROOT"]

# a FIFO at the card path must not hang --deploy mid-way, and a failed card is said out loud
import signal; signal.alarm(20)  # a blocked open must fail the test, not hang it
import contextlib, io
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "home/jandoe")); os.makedirs(os.path.join(root, "boot"))
os.mkfifo(os.path.join(root, "home/jandoe/schema-recovery.txt"))
os.environ["MIGRATE_ROOT"] = root
try:
    out = io.StringIO()
    with contextlib.redirect_stderr(out):
        paths = m.write_recovery_card({"user": "jandoe", "uid": os.getuid()}, root=root)
    check("FIFO: returns with the boot card only", paths == ["/boot/schema-recovery.txt"])
    check("failed home card is reported, with where the copy is",
          "schema-recovery.txt" in out.getvalue() and "/boot/" in out.getvalue())
finally:
    del os.environ["MIGRATE_ROOT"]

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
