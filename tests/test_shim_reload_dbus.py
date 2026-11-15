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

# audit 4.1: other services reload through PID 1 and restart only when it
# says cannot-reload
open(os.path.join(svc, "web.svc"), "w").write("name=web\nexec=/bin/true\n")
open(os.path.join(svc, "idle.svc"), "w").write("name=idle\nexec=/bin/true\n")
ctl2 = os.path.join(bin_, "fake-ctl2")
open(ctl2, "w").write("""#!/bin/sh
echo "$*" >> %s
case "$1" in
status) printf 'service.web.state=PERFECT\\nservice.idle.state=EXCISED\\n' ;;
reload) echo "$RELOAD_REPLY"; case "$RELOAD_REPLY" in ok:*) exit 0 ;; *) exit 1 ;; esac ;;
*) echo "ok: $*" ;;
esac
""" % ctllog)
os.chmod(ctl2, 0o755)
OKR = "ok: SIGHUP → web (pid 5)"
NOR = "err: cannot-reload: web has no exec_reload= or reload_signal="
for verb in ("reload", "reload-or-restart", "try-reload-or-restart"):
    rc, ctl, _ = run(verb, "web.service", SCHEMA_CTL=ctl2, RELOAD_REPLY=OKR)
    check("%s web (reloadable) -> reload, no restart" % verb,
          rc == 0 and "reload web" in ctl and "restart web" not in ctl, "ctl=%r" % ctl)
    rc, ctl, _ = run(verb, "web.service", SCHEMA_CTL=ctl2, RELOAD_REPLY=NOR)
    check("%s web (cannot-reload) -> restart" % verb,
          "reload web" in ctl and "restart web" in ctl, "ctl=%r" % ctl)
    rc, ctl, _ = run(verb, "idle.service", SCHEMA_CTL=ctl2, RELOAD_REPLY=OKR)
    check("%s idle (inactive) -> nothing" % verb, "idle" not in ctl.replace("status", ""), "ctl=%r" % ctl)
for verb in ("reload", "reload-or-restart"):
    r = subprocess.run([SHIM, verb, "web.service"], env=dict(env, SCHEMA_CTL=ctl2, RELOAD_REPLY="err: web is already reloading"),
                       capture_output=True, text=True)
    ctl = open(ctllog).read() if os.path.exists(ctllog) else ""
    check("%s: other reload errors -> exit 1 + stderr, never a restart" % verb,
          r.returncode == 1 and "already reloading" in r.stderr and "restart web" not in ctl,
          "rc=%d err=%r ctl=%r" % (r.returncode, r.stderr, ctl))
    os.unlink(ctllog)
r = subprocess.run([SHIM, "reload", "web.service"], env=dict(env, SCHEMA_CTL=ctl2, RELOAD_REPLY=OKR),
                   capture_output=True, text=True)
check("a successful reload prints nothing, like systemctl", r.returncode == 0 and r.stdout == "" and r.stderr == "",
      "out=%r err=%r" % (r.stdout, r.stderr))
rc, ctl, _ = run("reload", "web.service", SCHEMA_CTL=ctl2, RELOAD_REPLY="ok: reload (graceful)")
check("a supervisor older than reload <svc> (config-reload reply) -> restart",
      "reload web" in ctl and "restart web" in ctl, "ctl=%r" % ctl)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
