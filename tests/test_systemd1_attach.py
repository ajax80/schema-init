#!/usr/bin/env python3
# AttachProcessesToUnit: the root bridge moves a user's own podman process into
# a scope under that user's user@U.service, and nothing else.
import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("systemd1", os.path.join(HERE, "..", "scripts", "schema-systemd1.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

results = []
def check(name, ok):
    results.append(bool(ok)); print(("  PASS  " if ok else "  FAIL  ") + name)


def refused(fn, kind):
    try:
        fn()
    except m.AttachRefused as e:
        return e.kind == kind
    return False


me = os.getuid()
if me == 0:
    print("test_systemd1_attach: skipped (needs a non-root uid)")
    sys.exit(0)

root = tempfile.mkdtemp()
svc = "%s/user.slice/user-%d.slice/user@%d.service" % (root, me, me)
leaf = svc + "/user.slice/podman-1.scope"
os.makedirs(leaf)
procs = leaf + "/cgroup.procs"
open(procs, "w").close()
unit = "user@%d.service" % me
sub = "/user.slice/podman-1.scope"


def v(uid=me, u=unit, s=sub, pids=(123,)):
    return lambda: m.attach_validate(uid, u, s, list(pids), root)


check("valid request -> target dir", m.attach_validate(me, unit, sub, [123], root) == leaf)
check("root caller refused", refused(v(uid=0), "AccessDenied"))
check("other user's unit refused", refused(v(u="user@%d.service" % (me + 1)), "AccessDenied"))
for bad in ("/user.slice/../x", "/foo.scope", "/user.slice/app-1.scope", "/user.slice/podman-a/b.scope"):
    check("subcgroup %s refused" % bad, refused(v(s=bad), "InvalidArgs"))
check("no pids refused", refused(v(pids=()), "InvalidArgs"))
check("5 pids refused", refused(v(pids=(1, 2, 3, 4, 5)), "InvalidArgs"))
check("missing leaf refused", refused(v(s="/user.slice/podman-2.scope"), "InvalidArgs"))

# user.slice swapped for a symlink to a look-alike tree elsewhere
other = tempfile.mkdtemp()
os.makedirs(other + "/podman-1.scope")
os.rename(svc + "/user.slice", svc + "/user.slice.real")
os.symlink(other, svc + "/user.slice")
check("symlinked user.slice refused", refused(v(), "AccessDenied"))
os.unlink(svc + "/user.slice")
os.rename(svc + "/user.slice.real", svc + "/user.slice")

m.attach_pids(leaf, [os.getpid()], me)
check("own pid written", open(procs).read().strip() == str(os.getpid()))

open(procs, "w").close()
check("root's pid 1 refused", refused(lambda: m.attach_pids(leaf, [1], me), "AccessDenied"))
check("  ...nothing written", open(procs).read() == "")

p = subprocess.Popen(["true"]); p.wait()
check("exited pid refused", refused(lambda: m.attach_pids(leaf, [p.pid], me), "InvalidArgs"))
check("  ...nothing written", open(procs).read() == "")

real_exited = m._exited
m._exited = lambda fd: True
check("exited before write refused", refused(lambda: m.attach_pids(leaf, [os.getpid()], me), "InvalidArgs"))
check("  ...nothing written", open(procs).read() == "")

seq = iter([False, True])
m._exited = lambda fd: next(seq)
check("exited after write refused", refused(lambda: m.attach_pids(leaf, [os.getpid()], me), "InvalidArgs"))
check("  ...only the target was written", open(procs).read().strip() == str(os.getpid()))
m._exited = real_exited

print("test_systemd1_attach: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
