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
    check("no dep, even with udev-trigger present (DORMANT dep blocks forever)", "dep=" not in body)
    check("outlives its 120s /dev wait", "start_timeout_sec=300" in body)
    check("runs as a oneshot", "oneshot=1" in body)
    check("runs privileged (needs_root)", "needs_root=1" in body)
    check("non-critical (never wedges boot)", "critical=0" in body)
    check("recorded in the manifest", "/" + SVC_REL in man.files)
    dsvc = os.path.join(root, "etc/schema-init/services/schema-dbus-healthcheck.svc")
    dbody = open(dsvc).read() if os.path.exists(dsvc) else ""
    check("writes the dbus seatbelt .svc", dbody)
    check("dbus seatbelt execs the packaged healthcheck", "exec=" + m.DBUS_SEATBELT_HELPER in dbody)
    check("dbus seatbelt has no dep (a DORMANT dep would block it forever)", "dep=" not in dbody)
    check("dbus seatbelt outlives its 120s bus wait", "start_timeout_sec=300" in dbody)
    check("dbus seatbelt privileged oneshot, non-critical", all(k in dbody for k in ("oneshot=1", "needs_root=1", "critical=0")))
    check("dbus seatbelt recorded in the manifest", "/etc/schema-init/services/schema-dbus-healthcheck.svc" in man.files)
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
    open(os.path.join(root, "boot/loader/entries/f-6.10.0.conf"), "w").write(
        "title Fedora\nversion 6.10.0\noptions root=UUID=aaa ro\n")
    return root

root = _fedora_kde_root()
os.environ["MIGRATE_ROOT"] = root; os.environ["MIGRATE_KERNEL"] = "6.10.0"
m = _load()
try:
    m.main(["--deploy", "--prebuilt"], run=lambda *a, **k: type("R", (), {"returncode": 0, "stdout": ""})())
    svc = os.path.join(root, SVC_REL)
    check("do_deploy wires the seatbelt", os.path.exists(svc))
    check("do_deploy seatbelt has no dep", "dep=" not in open(svc).read())
    check("do_deploy records the advanced opt-outs in the stage", m.stage.read_extra(root).get("adv") == [])
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

# the healthcheck's bounded wait says so on screen instead of looking frozen
import shutil, subprocess

def _fake(bindir, name, body):
    path = os.path.join(bindir, name)
    open(path, "w").write("#!/bin/sh\n" + body)
    os.chmod(path, 0o755)

def _run_healthcheck(src, rootvar, statefile, probe, fails, plymouthd):
    tmp = tempfile.mkdtemp()
    lib, root, bindir = (os.path.join(tmp, d) for d in ("lib", "root", "bin"))
    for d in (lib, bindir, os.path.join(root, "dev"), os.path.join(root, "var/lib/schema-init"),
              os.path.join(root, "var/log/schema-init"), os.path.join(root, "etc/schema-init/services")):
        os.makedirs(d)
    script = os.path.join(lib, os.path.basename(src))
    shutil.copy2(os.path.join(REPO, src), script)
    for helper in ("schema-dbus-flip.sh", "schema-udev-flip-arm.sh", "schema-udev-flip-backup.sh"):
        _fake(lib, helper, "exit 0\n")
    open(os.path.join(root, "var/lib/schema-init", statefile), "w").write("armed\n")
    calls = os.path.join(tmp, "calls")
    _fake(bindir, "pgrep", 'case "$*" in *plymouthd*) exit %d ;; esac\nexit 0\n' % (0 if plymouthd else 1))
    _fake(bindir, "plymouth", 'printf "%%s\\n" "$*" >> %s\n' % calls)
    _fake(bindir, probe, 'n=$(cat %s.n 2>/dev/null || echo 0); n=$((n + 1)); echo $n > %s.n\n'
          '[ "$n" -gt %d ]\n' % (calls, calls, fails))
    for noop in ("sleep", "schema-ctl", "reboot"):
        _fake(bindir, noop, "exit 0\n")
    env = dict(os.environ, PATH=bindir + ":" + os.environ["PATH"], **{rootvar: root})
    subprocess.run(["sh", script], env=env, capture_output=True, timeout=30)
    console = os.path.join(root, "dev/console")
    con = open(console).read() if os.path.exists(console) else ""
    ply = open(calls).read() if os.path.exists(calls) else ""
    return con, ply

for src, rootvar, statefile, probe, msg in (
        ("scripts/schema-dbus-flip-healthcheck.sh", "SCHEMA_DBUS_FLIP_ROOT", "dbus-flip.state", "dbus-send",
         "Checking the message bus - this can take up to 2 minutes"),
        ("distros/fedora-installer/schema-udev-flip-healthcheck.sh", "SCHEMA_UDEV_FLIP_ROOT", "firstboot.state", "ls",
         "Checking devices - this can take up to 2 minutes")):
    name = os.path.basename(src)
    con, ply = _run_healthcheck(src, rootvar, statefile, probe, 0, False)
    check(name + ": ready at once shows nothing", con == "" and "display-message" not in ply)
    con, ply = _run_healthcheck(src, rootvar, statefile, probe, 2, False)
    check(name + ": waiting without plymouth writes the console once", con == msg + "\n")
    con, ply = _run_healthcheck(src, rootvar, statefile, probe, 2, True)
    check(name + ": waiting under plymouth shows it on the splash once",
          ply.count("display-message --text=" + msg) == 1 and con == "")
    check(name + ": the splash message is cleared after the wait", "hide-message --text=" + msg in ply)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
