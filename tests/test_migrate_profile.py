#!/usr/bin/env python3
"""profile assembly tests — fake fstab/passwd/os-release/BLS under MIGRATE_ROOT."""
import os, sys, json, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
os.environ["MIGRATE_KERNEL"] = "6.10.0-test.fc44.x86_64"
for d in ("etc", "usr/bin", "boot/loader/entries", "var/lib"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "etc/os-release"), "w").write('ID=fedora\nVERSION_ID=44\n')
open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
open(os.path.join(root, "etc/fstab"), "w").write(
    "# comment\nUUID=aaa / ext4 defaults 0 1\n"
    "UUID=bbb /home xfs defaults 0 2\nproc /proc proc defaults 0 0\n")
open(os.path.join(root, "etc/passwd"), "w").write(
    "root:x:0:0:root:/root:/bin/bash\njandoe:x:1000:1000:Jan:/home/jandoe:/bin/bash\n")
open(os.path.join(root, "boot/loader/entries/x.conf"), "w").write("title Fedora\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

m = sm.read_fstab()
check("fstab skips comments + pseudo-fs", len(m) == 2 and all(x["fstype"] != "proc" for x in m))
check("fstab captures /home xfs", any(x["target"] == "/home" and x["fstype"] == "xfs" for x in m))
u, uid = sm.primary_user()
check("primary user is uid 1000", u == "jandoe" and uid == 1000)
check("bootloader detected as bls", sm.bootloader_kind() == "bls")

prof = sm.build_profile(run=lambda *a, **k: type("R", (), {"stdout": "", "returncode": 0})())
check("profile carries platform", prof["platform"] == "fedora-kde")
check("profile carries kernel from env", prof["kernel"] == "6.10.0-test.fc44.x86_64")
check("profile has mounts + user + services keys",
      "mounts" in prof and prof["user"] == "jandoe" and "services" in prof)

path = sm.write_profile(prof)
check("profile json written", os.path.exists(path) and json.load(open(path))["platform"] == "fedora-kde")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
