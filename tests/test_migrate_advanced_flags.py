#!/usr/bin/env python3
"""migrate accepts the wizard's --advanced-* flags and acts on each one."""
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
open(os.path.join(root, "boot/loader/entries/f-6.10.0.conf"), "w").write(
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

def fresh_root():
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
    open(os.path.join(root, "boot/loader/entries/f-6.10.0.conf"), "w").write(
        "title Fedora\nversion 6.10.0\noptions root=UUID=a ro\n")
    return root

def deploy(flags, fstype="", snap_rc=0):
    root = fresh_root()
    calls = []
    def run(cmd, *a, **k):
        calls.append(list(cmd))
        if cmd[:1] == ["findmnt"]:
            return type("R", (), {"returncode": 0, "stdout": fstype + "\n"})()
        if cmd[:3] == ["btrfs", "subvolume", "snapshot"]:
            return type("R", (), {"returncode": snap_rc, "stdout": ""})()
        return type("R", (), {"returncode": 0, "stdout": ""})()
    os.environ.pop("MIGRATE_ADV", None)
    os.environ["MIGRATE_ROOT"] = root; os.environ["MIGRATE_KERNEL"] = "6.10.0"
    m = load()
    err = None
    try:
        m.main(["--deploy", "--prebuilt"] + flags, run=run)
    except RuntimeError as e:
        err = e
    finally:
        del os.environ["MIGRATE_ROOT"]; del os.environ["MIGRATE_KERNEL"]
        os.environ.pop("MIGRATE_ADV", None)
    return m, root, calls, err

def snapshots(calls):
    return [c for c in calls if c[:3] == ["btrfs", "subvolume", "snapshot"]]

# --advanced-no-snapshot
m, root, calls, err = deploy([], fstype="btrfs")
check("btrfs root: deploy takes a read-only snapshot first", len(snapshots(calls)) == 1
      and snapshots(calls)[0][3] == "-r" and calls.index(snapshots(calls)[0]) < calls.index(
          next(c for c in calls if c[:1] == ["dnf"] or c[:1] == ["grub2-editenv"])))
snap = snapshots(calls)[0][5]
man = m.Manifest.load()
check("snapshot recorded in the manifest", man.snapshot == "/" + os.path.relpath(snap, root))
card = open(os.path.join(root, "boot/schema-recovery.txt")).read()
check("recovery card names the snapshot", man.snapshot in card)
os.makedirs(snap)
ucalls = []
os.environ["MIGRATE_ROOT"] = root
try:
    m = load(); m.uninstall(run=lambda cmd, *a, **k: ucalls.append(list(cmd)) or type("R", (), {"returncode": 0, "stdout": ""})())
finally:
    del os.environ["MIGRATE_ROOT"]
check("uninstall deletes the snapshot", ["btrfs", "subvolume", "delete", snap] in ucalls)
m, root, calls, err = deploy(["--advanced-no-snapshot"], fstype="btrfs")
check("no-snapshot: no snapshot taken", snapshots(calls) == [] and err is None)
check("no-snapshot: card mentions no snapshot", "read-only copy" not in open(os.path.join(root, "boot/schema-recovery.txt")).read())
m, root, calls, err = deploy([], fstype="ext4")
check("non-btrfs root: snapshot skipped, deploy goes on", snapshots(calls) == [] and err is None
      and m.stage.read_stage(root) == m.stage.R1_PENDING)
m, root, calls, err = deploy([], fstype="btrfs", snap_rc=1)
check("failed snapshot refuses before changing anything", err is not None
      and not os.path.exists(os.path.join(root, m.Manifest.PATH))
      and not os.path.exists(os.path.join(root, "boot/loader/entries/schema-6.10.0.conf")))

# --advanced-no-fallback-entry
m, root, calls, err = deploy([])
ents = sorted(os.listdir(os.path.join(root, "boot/loader/entries")))
check("default keeps the current system's boot entry", ents == ["f-6.10.0.conf", "schema-6.10.0.conf"])
check("default card still points at the old entry", "does NOT say" in open(os.path.join(root, "boot/schema-recovery.txt")).read())
m, root, calls, err = deploy(["--advanced-no-fallback-entry"])
ents = sorted(os.listdir(os.path.join(root, "boot/loader/entries")))
check("no-fallback-entry: only the schema entry is left", ents == ["schema-6.10.0.conf"])
card = open(os.path.join(root, "boot/schema-recovery.txt")).read()
check("no-fallback-entry: card does not point at a hidden menu entry", "does NOT say" not in card
      and "no menu" in card)
check("no-fallback-entry: the old entry is stashed, not deleted",
      os.path.exists(os.path.join(root, m.FALLBACK_STASH, "f-6.10.0.conf")))
os.environ["MIGRATE_ROOT"] = root
try:
    m = load(); m.uninstall(run=lambda *a, **k: type("R", (), {"returncode": 0, "stdout": ""})())
finally:
    del os.environ["MIGRATE_ROOT"]
check("uninstall puts the old entry back", sorted(os.listdir(os.path.join(root, "boot/loader/entries"))) == ["f-6.10.0.conf"])

# --advanced-no-doctor-timers
SVC = "etc/schema-init/services/"
m, root, calls, err = deploy([])
check("default installs the periodic doctor", os.path.exists(os.path.join(root, SVC, "schema-doctor-periodic.svc")))
m, root, calls, err = deploy(["--advanced-no-doctor-timers"])
check("no-doctor-timers: periodic doctor not installed", not os.path.exists(os.path.join(root, SVC, "schema-doctor-periodic.svc")))
check("no-doctor-timers: boot-time doctor still installed (migrate-finish deps on it)",
      os.path.exists(os.path.join(root, SVC, "schema-doctor.svc")))

# --advanced-no-udev-flip skips the R2 offer the way no-dbus-broker skips R3
def at_r1_heal(adv):
    root = tempfile.mkdtemp()
    m = load()
    m.stage.write_stage(m.stage.R1_HEAL, root=root, extra={"adv": adv})
    auto = os.path.join(root, m.AUTOSTART)
    os.makedirs(os.path.dirname(auto)); open(auto, "w").close()
    flips = []
    def flip(*a):
        flips.append(a); return type("R", (), {"returncode": 0, "stdout": ""})()
    return m, root, flips, flip, auto

m, root, flips, flip, auto = at_r1_heal([])
rc = m.arm_flip(root=root, flip=flip)
check("default arm-flip arms the udev flip (R2)", rc == 0 and flips == [("arm",)]
      and m.stage.read_stage(root) == m.stage.R2_PENDING)
m, root, flips, flip, auto = at_r1_heal(["no-udev-flip"])
rc = m.arm_flip(root=root, flip=flip)
check("no-udev-flip: nothing armed, straight to DONE", rc == 0 and flips == []
      and m.stage.read_stage(root) == m.stage.DONE)
check("no-udev-flip: the dbus offer still follows", m.dbus_offered(root) and os.path.exists(auto))
m, root, flips, flip, auto = at_r1_heal(["no-udev-flip", "no-dbus-broker"])
m.arm_flip(root=root, flip=flip)
check("no-udev-flip + no-dbus-broker: wizard torn down", m.stage.read_stage(root) == m.stage.DONE
      and not os.path.exists(auto))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
