#!/usr/bin/env python3
"""migrate accepts the wizard's --advanced-* flags without error."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

def load():
    spec = importlib.util.spec_from_file_location("schema_migrate_af", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

root = tempfile.mkdtemp()
for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib", "home/j", "usr/lib/systemd"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "etc/os-release"), "w").write("ID=fedora\n")
open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
for b in ("schema-init", "schema-ctl", "schema-subreaper"):
    open(os.path.join(root, "usr/bin", b), "w").close()
open(os.path.join(root, "usr/lib/systemd/systemd-udevd"), "w").close()
open(os.path.join(root, "etc/fstab"), "w").write("UUID=a / ext4 defaults 0 1\n")
open(os.path.join(root, "etc/passwd"), "w").write("j:x:1000:1000::/home/j:/bin/sh\n")
open(os.path.join(root, "boot/loader/entries/f.conf"), "w").write(
    "title Fedora\nversion 6.10.0\noptions root=UUID=a ro\n")
os.environ["MIGRATE_ROOT"] = root; os.environ["MIGRATE_KERNEL"] = "6.10.0"
m = load()
try:
    rc = m.main(["--deploy", "--prebuilt", "--advanced-no-snapshot", "--advanced-no-doctor-timers"],
                run=lambda *a, **k: type("R", (), {"returncode": 0, "stdout": ""})())
    check("advanced flags accepted, deploy returns 0", rc == 0)
    check("stage advanced to R1_PENDING", m.stage.read_stage(root) == m.stage.R1_PENDING)
finally:
    del os.environ["MIGRATE_ROOT"]; del os.environ["MIGRATE_KERNEL"]

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
