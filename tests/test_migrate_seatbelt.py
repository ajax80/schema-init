#!/usr/bin/env python3
"""flip seatbelt rail-wiring tests — script-style."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_sb", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

SVC_REL = "etc/schema-init/services/schema-udev-healthcheck.svc"

# with udev-trigger present, the seatbelt is ordered after it and runs privileged
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "etc/schema-init/services"))
open(os.path.join(root, "etc/schema-init/services/udev-trigger.svc"), "w").close()
os.environ["MIGRATE_ROOT"] = root
m = _load()
try:
    man = m.Manifest()
    m.install_flip_seatbelt(man)
    svc = os.path.join(root, SVC_REL)
    check("writes the seatbelt .svc", os.path.exists(svc))
    body = open(svc).read()
    check("exec is the packaged healthcheck", "exec=" + m.SEATBELT_HELPER in body)
    check("ordered after udev-trigger", "dep=udev-trigger" in body)
    check("runs as a oneshot", "oneshot=1" in body)
    check("runs privileged (needs_root)", "needs_root=1" in body)
    check("non-critical (never wedges boot)", "critical=0" in body)
    check("recorded in the manifest", "/" + SVC_REL in man.files)
finally:
    del os.environ["MIGRATE_ROOT"]

# without udev-trigger, no dangling dep is emitted (schema-init would block on it)
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "etc/schema-init/services"))
os.environ["MIGRATE_ROOT"] = root
m = _load()
try:
    m.install_flip_seatbelt(m.Manifest())
    body = open(os.path.join(root, SVC_REL)).read()
    check("no dep when udev-trigger absent", "dep=" not in body)
finally:
    del os.environ["MIGRATE_ROOT"]

# do_deploy actually wires the seatbelt into the rail, ordered after udev-trigger
def _fedora_kde_root():
    root = tempfile.mkdtemp()
    for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib", "home/jandoe",
              "usr/lib/systemd"):
        os.makedirs(os.path.join(root, d))
    open(os.path.join(root, "etc/os-release"), "w").write("ID=fedora\n")
    open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
    for b in ("schema-init", "schema-ctl", "schema-subreaper"):
        open(os.path.join(root, "usr/bin", b), "w").close()
    open(os.path.join(root, "usr/lib/systemd/systemd-udevd"), "w").close()  # so udev-trigger is generated
    open(os.path.join(root, "etc/fstab"), "w").write("UUID=aaa / ext4 defaults 0 1\n")
    open(os.path.join(root, "etc/passwd"), "w").write("jandoe:x:1000:1000::/home/jandoe:/bin/bash\n")
    open(os.path.join(root, "boot/loader/entries/f.conf"), "w").write(
        "title Fedora\nversion 6.10.0\noptions root=UUID=aaa ro\n")
    return root

root = _fedora_kde_root()
os.environ["MIGRATE_ROOT"] = root; os.environ["MIGRATE_KERNEL"] = "6.10.0"
m = _load()
try:
    m.main(["--deploy", "--prebuilt"], run=lambda *a, **k: type("R", (), {"returncode": 0, "stdout": ""})())
    svc = os.path.join(root, SVC_REL)
    check("do_deploy wires the seatbelt", os.path.exists(svc))
    check("do_deploy orders it after udev-trigger", "dep=udev-trigger" in open(svc).read())
finally:
    del os.environ["MIGRATE_ROOT"]; del os.environ["MIGRATE_KERNEL"]

# dry-run writes nothing, records nothing
root = tempfile.mkdtemp()
os.environ["MIGRATE_ROOT"] = root
m = _load()
try:
    man = m.Manifest()
    m.install_flip_seatbelt(man, dry_run=True)
    check("dry-run writes no .svc", not os.path.exists(os.path.join(root, SVC_REL)))
    check("dry-run records nothing", man.files == [])
finally:
    del os.environ["MIGRATE_ROOT"]

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
