#!/usr/bin/env python3
"""systemctl reload dbus.service from a package scriptlet must reach the bus as
ReloadConfig, never as `schema-ctl restart dbus` (a live restart of the system
bus mid-transaction)."""
import os, subprocess, sys, tempfile
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIM = os.path.join(REPO, "schema-systemctl")

results = []
def check(name, cond, detail=""):
    results.append(bool(cond)); print(("  PASS  " if cond else "  FAIL  ") + name + (("  — " + detail) if detail and not cond else ""))

subprocess.run(["make", "-s", "-C", REPO, "schema-systemctl"], check=True)
t = tempfile.mkdtemp()
svc = os.path.join(t, "services"); os.makedirs(svc)
open(os.path.join(svc, "dbus.svc"), "w").write("name=dbus\nexec=/usr/libexec/schema-init/schema-dbus-run.sh\n")
comm = os.path.join(t, "comm"); open(comm, "w").write("schema-init\n")
bin_ = os.path.join(t, "bin"); os.makedirs(bin_)
ctllog, buslog = os.path.join(t, "ctl.log"), os.path.join(t, "gdbus.log")
for name, log, rc in (("fake-ctl", ctllog, 0), ("gdbus", buslog, 0)):
    p = os.path.join(bin_, name)
    open(p, "w").write('#!/bin/sh\necho "$*" >> %s\necho "()"\nexit ${FAKE_RC:-%d}\n' % (log, rc))
    os.chmod(p, 0o755)
env = dict(os.environ, PATH=bin_ + ":" + os.environ["PATH"], SCHEMA_SVC_DIR=svc,
           SCHEMA_CTL=os.path.join(bin_, "fake-ctl"), SCHEMA_PID1_COMM=comm,
           SCHEMA_SHM_PATH=os.path.join(t, "no-shm"))

def run(*args, **kw):
    for f in (ctllog, buslog):
        if os.path.exists(f): os.unlink(f)
    r = subprocess.run([SHIM] + list(args), env=dict(env, **kw), capture_output=True, text=True)
    read = lambda f: open(f).read() if os.path.exists(f) else ""
    return r.returncode, read(ctllog), read(buslog)

for verb, unit in (("reload", "dbus.service"), ("reload-or-restart", "dbus.service"),
                   ("try-reload-or-restart", "dbus.service"), ("reload", "dbus-broker.service"),
                   ("reload", "dbus")):
    rc, ctl, bus = run(verb, unit)
    check("%s %s -> ReloadConfig, no restart" % (verb, unit),
          rc == 0 and "restart" not in ctl and "org.freedesktop.DBus.ReloadConfig" in bus and "--system" in bus,
          "rc=%d ctl=%r bus=%r" % (rc, ctl, bus))

rc, ctl, bus = run("reload", "dbus.service", "foo.service")
check("mixed list: bus reloaded, never restarted", "org.freedesktop.DBus.ReloadConfig" in bus
      and "restart dbus" not in ctl, "ctl=%r bus=%r" % (ctl, bus))

rc, ctl, bus = run("reload", "dbus.service", FAKE_RC="1")
check("failed ReloadConfig -> nonzero exit", rc != 0, "rc=%d" % rc)

r = subprocess.run([SHIM, "reload", "dbus.service"], env=env, capture_output=True, text=True)
check("ReloadConfig reply not printed", r.returncode == 0 and r.stdout == "", "out=%r" % r.stdout)

os.rename(os.path.join(bin_, "gdbus"), os.path.join(bin_, "dbus-send"))
rc, ctl, bus = run("reload", "dbus.service", PATH=bin_)   # bin_ only: the host's gdbus must not answer
check("no gdbus -> dbus-send ReloadConfig", rc == 0 and "org.freedesktop.DBus.ReloadConfig" in bus
      and "--system" in bus, "rc=%d bus=%r" % (rc, bus))
os.rename(os.path.join(bin_, "dbus-send"), os.path.join(bin_, "gdbus"))

rc, ctl, bus = run("restart", "dbus.service")
check("explicit restart dbus still goes to schema-ctl", "restart dbus" in ctl and bus == "", "ctl=%r" % ctl)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
