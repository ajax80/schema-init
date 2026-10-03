#!/usr/bin/env python3
"""BLS boot-entry tests — fake /boot/loader/entries under MIGRATE_ROOT, real hook."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
ENTRIES = os.path.join(root, "boot/loader/entries"); os.makedirs(ENTRIES)
for v in ("6.10.0", "6.11.2"):
    open(os.path.join(ENTRIES, "tok-%s.conf" % v), "w").write(
        "title Fedora Linux 44 (%s)\nversion %s\nlinux /vmlinuz-%s\n"
        "initrd /initramfs-%s.img\noptions root=UUID=aaa ro quiet\n" % (v, v, v, v))
open(os.path.join(ENTRIES, "tok-0-rescue.conf"), "w").write(
    "title Fedora rescue\nversion 0-rescue\noptions root=UUID=aaa ro\n")
open(os.path.join(ENTRIES, "schema-init.conf"), "w").write("title old\noptions init=/usr/bin/schema-init\n")
os.makedirs(os.path.join(root, "etc/schema-init/kernel-cmdline.d"))
open(os.path.join(root, "etc/schema-init/kernel-cmdline.d/10-x.conf"), "w").write("modprobe.blacklist=radeon\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

calls, env = [], {"saved_entry": "tok-6.11.2"}
def fake_run(argv, **kw):
    calls.append(argv)
    if argv[:2] == ["grub2-editenv", "list"]:
        return type("R", (), {"returncode": 0, "stdout": "".join("%s=%s\n" % kv for kv in env.items())})()
    if argv[:3] == ["grub2-editenv", "-", "set"]:
        k, _, v = argv[3].partition("="); env[k] = v
    return type("R", (), {"returncode": 0, "stdout": ""})()

check("stock entries keyed by version, rescue skipped",
      sm.stock_entries() == {"6.10.0": "tok-6.10.0", "6.11.2": "tok-6.11.2"})

m = sm.Manifest()
path = sm.seed_boot_entries("6.10.0", m, run=fake_run)
body = open(path).read()
check("entry for the running kernel", path.endswith("schema-6.10.0.conf"))
check("entry for every other installed kernel", os.path.exists(os.path.join(ENTRIES, "schema-6.11.2.conf")))
check("title marks schema-init", "(schema-init)" in body)
check("options carry init=/usr/bin/schema-init", "init=/usr/bin/schema-init" in body)
check("options carry kernel-cmdline.d extras", "modprobe.blacklist=radeon" in body)
check("keeps original kernel/root", "root=UUID=aaa" in body and "/vmlinuz-6.10.0" in body)
check("no rescue clone", not [e for e in os.listdir(ENTRIES) if e.startswith("schema-") and "rescue" in e])
check("legacy schema-init.conf removed", not os.path.exists(os.path.join(ENTRIES, "schema-init.conf")))
orig = open(os.path.join(ENTRIES, "tok-6.10.0.conf")).read()
check("original entry untouched", "(schema-init)" not in orig and "init=" not in orig)
check("hook installed", os.access(os.path.join(root, sm.HOOK_REL), os.X_OK) and "/" + sm.HOOK_REL in m.files)
check("boot-default marker armed, empty (no pin)",
      open(os.path.join(root, sm.BOOT_DEFAULT)).read() == "" and "/" + sm.BOOT_DEFAULT in m.files)
check("default is the RUNNING kernel's entry, not the newest", env["saved_entry"] == "schema-6.10.0")
check("prior default recorded", m.grub["saved_entry_was"] == "tok-6.11.2")

sm.seed_boot_entries("6.10.0", m, run=fake_run)
check("init= not doubled on re-seed", open(path).read().count("init=/usr/bin/schema-init") == 1)

sm.remove_boot_entries(m.grub["saved_entry_was"], run=fake_run)
check("remove deletes every schema entry",
      not [e for e in os.listdir(ENTRIES) if e.startswith("schema-")])
check("remove keeps stock entries", sorted(os.listdir(ENTRIES)) == ["tok-0-rescue.conf", "tok-6.10.0.conf", "tok-6.11.2.conf"])
check("remove repoints default at the stock twin", env["saved_entry"] == "tok-6.10.0")

try:
    sm.seed_boot_entries("9.9.9", sm.Manifest(), run=fake_run); ok = False
except RuntimeError:
    ok = True
check("refuses when the running kernel got no entry", ok)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
