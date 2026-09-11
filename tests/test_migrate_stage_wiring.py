#!/usr/bin/env python3
"""--deploy / --stage CLI wiring tests — script-style."""
import os, sys, io, tempfile, importlib.util, contextlib
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_sw", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

def _fedora_kde_root():
    root = tempfile.mkdtemp()
    for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib", "home/jandoe"):
        os.makedirs(os.path.join(root, d))
    open(os.path.join(root, "etc/os-release"), "w").write("ID=fedora\n")
    open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
    for b in ("schema-init", "schema-ctl", "schema-subreaper"):
        open(os.path.join(root, "usr/bin", b), "w").close()  # prebuilt present
    open(os.path.join(root, "etc/fstab"), "w").write("UUID=aaa / ext4 defaults 0 1\n")
    open(os.path.join(root, "etc/passwd"), "w").write("jandoe:x:1000:1000::/home/jandoe:/bin/bash\n")
    open(os.path.join(root, "boot/loader/entries/f.conf"), "w").write(
        "title Fedora\nversion 6.10.0\noptions root=UUID=aaa ro\n")
    return root

# --deploy advances stage to R1_PENDING and writes the recovery card
root = _fedora_kde_root()
os.environ["MIGRATE_ROOT"] = root; os.environ["MIGRATE_KERNEL"] = "6.10.0"
m = _load()
try:
    rc = m.main(["--deploy", "--prebuilt"], run=lambda *a, **k: type("R", (), {"returncode": 0, "stdout": ""})())
    check("--deploy returns 0", rc == 0)
    check("--deploy sets R1_PENDING", m.stage.read_stage(root) == m.stage.R1_PENDING)
    check("--deploy writes recovery card", os.path.exists(os.path.join(root, "home/jandoe/schema-recovery.txt")))
    fin = os.path.join(root, "etc/schema-init/services/schema-migrate-finish.svc")
    check("--deploy installs the finish oneshot", os.path.exists(fin))
    finbody = open(fin).read() if os.path.exists(fin) else ""
    check("finish oneshot runs schema-migrate --finish",
          "args=--finish" in finbody)
    check("finish oneshot execs the installed schema-migrate path",
          "exec=/usr/bin/schema-migrate\n" in finbody)
finally:
    del os.environ["MIGRATE_ROOT"]; del os.environ["MIGRATE_KERNEL"]

# --stage prints the current stage
root = _fedora_kde_root(); os.environ["MIGRATE_ROOT"] = root
m = _load()
try:
    m.stage.write_stage(m.stage.R1_HEAL, root=root)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        m.main(["--stage"])
    check("--stage prints current stage", "R1_HEAL" in buf.getvalue())
finally:
    del os.environ["MIGRATE_ROOT"]

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
