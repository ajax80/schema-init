#!/usr/bin/env python3
"""host unit generation tests."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

profile = {"mounts": [
    {"src": "UUID=aaa", "target": "/", "fstype": "ext4", "opts": "defaults"},
    {"src": "UUID=boo", "target": "/boot", "fstype": "ext4", "opts": "defaults"},
    {"src": "UUID=efi", "target": "/boot/efi", "fstype": "vfat", "opts": "umask=0077"},
    {"src": "UUID=bbb", "target": "/home", "fstype": "xfs", "opts": "defaults"},
    {"src": "UUID=ccc", "target": "/mnt/data", "fstype": "ext4", "opts": "defaults"}]}
m = sm.Manifest()
dst = sm.generate_host_units(profile, m)
home_unit = os.path.join(root, "etc/schema-init/services/mount-home.svc")
data_unit = os.path.join(root, "etc/schema-init/services/mount-mnt-data.svc")
root_unit = os.path.join(root, "etc/schema-init/services/mount-root.svc")
boot_unit = os.path.join(root, "etc/schema-init/services/mount-boot.svc")
efi_unit = os.path.join(root, "etc/schema-init/services/mount-boot-efi.svc")
check("home mount unit written", os.path.exists(home_unit))
check("nested mount slug is path-safe", os.path.exists(data_unit))
check("no unit generated for /", not os.path.exists(root_unit))
check("manifest does not record /", not any("mount-root.svc" in f for f in m.files))
check("/boot mount unit written", os.path.exists(boot_unit))
check("/boot/efi mount unit written", os.path.exists(efi_unit))
check("efi depends on parent /boot mount", "dep=mount-boot\n" in open(efi_unit).read())
check("non-nested mount has no parent dep", "dep=mount-" not in open(home_unit).read())
body = open(home_unit).read()
check("unit mounts the right target", "/home" in body and "UUID=bbb" in body)
check("unit is oneshot needs_root", "oneshot=1" in body and "needs_root=1" in body)
check("unit waits for udev by-uuid (dep=udev-trigger)", "dep=udev-trigger" in body)
# schema-init takes one argv element per args= line — a combined line reaches
# /bin/mount as a single garbage argument and the mount silently fails.
alines = [l for l in body.splitlines() if l.startswith("args=")]
check("each mount arg on its own args= line", alines == [
    "args=-t", "args=xfs", "args=-o", "args=defaults", "args=UUID=bbb", "args=/home"])
check("manifest recorded units", any("mount-home.svc" in f for f in m.files))

# NM DNS: under schema-init resolved is absent, so resolv.conf must be a real
# file NM writes, not a dangling symlink to resolved's stub.
os.makedirs(os.path.join(root, "etc"), exist_ok=True)
rc = os.path.join(root, "etc/resolv.conf")
try: os.remove(rc)
except OSError: pass
os.symlink("../run/systemd/resolve/stub-resolv.conf", rc)
sm.generate_nm_config(sm.Manifest())
nmconf = open(os.path.join(root, "etc/NetworkManager/conf.d/10-schema-managed.conf")).read()
check("NM writes resolv.conf as a real file (rc-manager=file)", "rc-manager=file" in nmconf)
check("dangling resolved symlink removed", not os.path.islink(rc))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
