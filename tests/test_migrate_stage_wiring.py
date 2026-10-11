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
    open(os.path.join(root, "boot/loader/entries/f-6.10.0.conf"), "w").write(
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


# --retry / --why after a rollback
import json as _json
class _R:
    def __init__(self, rc=0, out=""): self.returncode = rc; self.stdout = out
def _flip_log():
    calls = []
    def f(*a):
        calls.append(a)
        return _R(0, "system bus is not schema-dbus (launcher fell back" if a[0].endswith("explain") else "")
    return f, calls
rr = tempfile.mkdtemp(); os.makedirs(os.path.join(rr, "home/jandoe")); os.makedirs(os.path.join(rr, "var/lib/schema-init"))
_json.dump({"user": "jandoe", "uid": os.getuid()}, open(os.path.join(rr, "var/lib/schema-init/migrate-profile.json"), "w"))
sm = _load(); stg = sm.stage
stg.write_stage(stg.R3_ROLLED_BACK, root=rr)
f, calls = _flip_log()
check("--why reads the dbus seatbelt's reason", "launcher fell back" in sm.rollback_reason(root=rr, flip=f) and calls[-1] == ("dbus-explain",))
check("retry re-arms the dbus flip", sm.retry_flip(root=rr, flip=f) == 0 and ("dbus-arm",) in calls)
check("retry moves R3_ROLLED_BACK -> R3_PENDING, armed this boot",
      stg.read_stage(rr) == stg.R3_PENDING and stg.read_extra(rr).get("dbus_armed_boot") == sm._boot_id())
au = os.path.join(rr, "home/jandoe", sm.USER_AUTOSTART)
check("retry restores the wizard's login autostart", os.path.exists(au) and "Exec=/usr/bin/schema-wizard" in open(au).read())
victim = os.path.join(rr, "victim"); open(victim, "w").write("secret\n")
os.remove(au); os.symlink(victim, au)
stg.write_stage(stg.R3_ROLLED_BACK, root=rr)
sm.retry_flip(root=rr, flip=_flip_log()[0])
check("autostart write never follows a planted symlink", open(victim).read() == "secret\n")
os.remove(au)
stg.write_stage(stg.ROLLED_BACK, root=rr)
f, calls = _flip_log()
check("udev retry re-arms the udev flip", sm.retry_flip(root=rr, flip=f) == 0 and ("arm",) in calls and stg.read_stage(rr) == stg.R2_PENDING)
check("nothing to retry outside a rollback", sm.retry_flip(root=rr, flip=f) == 1)
stg.write_stage(stg.R3_ROLLED_BACK, root=rr)
check("a failed arm leaves the stage alone", sm.retry_flip(root=rr, flip=lambda *a: _R(1)) == 1 and stg.read_stage(rr) == stg.R3_ROLLED_BACK)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
