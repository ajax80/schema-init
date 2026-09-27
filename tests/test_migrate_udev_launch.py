#!/usr/bin/env python3
"""migrate rail: the LIVE flag picks schema-udev vs systemd-udevd at boot.

Before this, --deploy wrote udevd.svc as plain systemd-udevd and nothing ever
started schema-udev, so an armed udev flip could only roll back. Runs the
generated launcher and trigger scripts with their fixed paths rewritten into
a temp root and fake binaries that record themselves.

  ./tests/test_migrate_udev_launch.py     exit 0 all pass, 1 any fail
"""
import os, sys, tempfile, subprocess, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []
def check(name, cond, detail=""):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name + (f"  — {detail}" if detail else ""))

root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "usr/lib/systemd"))
open(os.path.join(root, "usr/lib/systemd/systemd-udevd"), "w").close()
os.environ["MIGRATE_ROOT"] = root
spec = importlib.util.spec_from_file_location("sm_udev", os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
man = m.Manifest()
m.generate_udev_units(man)

svc = open(os.path.join(root, "etc/schema-init/services/udevd.svc")).read()
check("udevd.svc execs the launcher", "exec=" + m.UDEVD_LAUNCH in svc)
check("launcher recorded in the manifest", m.UDEVD_LAUNCH in man.files)

fake = os.path.join(root, "fake")
os.makedirs(os.path.join(fake, "bin"))
os.makedirs(os.path.join(fake, "etc"))
os.makedirs(os.path.join(fake, "run/schema-udev"))
log = os.path.join(fake, "ran")
for name in ("schema-udev", "systemd-udevd", "udevadm"):
    p = os.path.join(fake, "bin", name)
    open(p, "w").write('#!/bin/sh\necho "%s $*" >> %s\n' % (name, log))
    os.chmod(p, 0o755)
flag = os.path.join(fake, "etc/schema-udev.live")

def rewrite(rel):
    body = open(os.path.join(root, rel)).read()
    body = (body.replace(m.UDEV_LIVE_FLAG, flag)
                .replace("/usr/bin/schema-udev", os.path.join(fake, "bin/schema-udev"))
                .replace("/usr/lib/systemd/systemd-udevd", os.path.join(fake, "bin/systemd-udevd"))
                .replace("/run/schema-udev", os.path.join(fake, "run/schema-udev"))
                .replace("udevadm ", os.path.join(fake, "bin/udevadm") + " "))
    out = os.path.join(fake, os.path.basename(rel))
    open(out, "w").write(body); os.chmod(out, 0o755)
    return out

launch = rewrite(m.UDEVD_LAUNCH.lstrip("/"))
trigger = rewrite("usr/local/bin/schema-udev-trigger.sh")

def ran(script):
    if os.path.exists(log): os.remove(log)
    subprocess.run([script], timeout=60)
    return open(log).read().split("\n") if os.path.exists(log) else []

print("-- disarmed --")
out = ran(launch)
check("launcher runs systemd-udevd", out[0].startswith("systemd-udevd"), str(out))
out = ran(trigger)
check("trigger runs udevadm trigger + settle", sum(l.startswith("udevadm") for l in out) == 3, str(out))

print("-- armed --")
open(flag, "w").close()
out = ran(launch)
check("launcher runs schema-udev", out[0].startswith("schema-udev"), str(out))
open(os.path.join(fake, "run/schema-udev/ready"), "w").close()
out = ran(trigger)
check("trigger leaves coldplug to schema-udev", not any(l.startswith("udevadm") for l in out), str(out))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
