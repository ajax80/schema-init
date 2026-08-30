#!/usr/bin/env python3
"""BLS boot-entry tests — fake /boot/loader/entries under MIGRATE_ROOT."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
ENTRIES = os.path.join(root, "boot/loader/entries"); os.makedirs(ENTRIES)
open(os.path.join(ENTRIES, "fedora-6.10.0.conf"), "w").write(
    "title Fedora Linux 44\nversion 6.10.0\nlinux /vmlinuz-6.10.0\n"
    "initrd /initramfs-6.10.0.img\noptions root=UUID=aaa ro quiet\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

path = sm.add_boot_entry("6.10.0")
body = open(path).read()
check("schema entry created", path.endswith("schema-init.conf") and os.path.exists(path))
check("title marks schema-init", "(schema-init)" in body)
check("options carry init=/usr/bin/schema-init", "init=/usr/bin/schema-init" in body)
check("keeps original kernel/root", "root=UUID=aaa" in body and "/vmlinuz-6.10.0" in body)
orig = open(os.path.join(ENTRIES, "fedora-6.10.0.conf")).read()
check("original entry untouched", "(schema-init)" not in orig and "init=/usr/bin/schema-init" not in orig)

sm.add_boot_entry("6.10.0")  # idempotent
check("init= not doubled on re-add", open(path).read().count("init=/usr/bin/schema-init") == 1)

sm.remove_boot_entry()
check("remove deletes schema entry", not os.path.exists(path))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
