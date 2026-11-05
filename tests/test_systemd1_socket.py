#!/usr/bin/env python3
# The bridge's private socket (/run/systemd/private) is bound by dbus at mode
# 0777 and narrowed to 0600 only afterward; a connection in that window has
# sender=None, which authorize() trusts as root. _bind_private_socket must never
# expose a world-connectable socket at the real path, and must never change the
# shared parent dir's mode (so an abnormal exit can't corrupt /run/systemd).
import glob
import importlib.util
import os
import stat
import sys
import tempfile

try:
    import dbus.mainloop.glib
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
except ImportError as e:
    print("test_systemd1_socket: skipped (%s)" % e)
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "systemd1", os.path.join(HERE, "..", "scripts", "schema-systemd1.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

results = []
def check(name, ok):
    results.append(bool(ok)); print(("  PASS  " if ok else "  FAIL  ") + name)


def mode(p):
    return stat.S_IMODE(os.stat(p).st_mode)


root = tempfile.mkdtemp()
sockdir = os.path.join(root, "systemd")
os.mkdir(sockdir)
before = mode(sockdir)          # whatever the umask produced; compare against it
path = os.path.join(sockdir, "private")

srv = m._bind_private_socket(path)
check("socket created at the real path", os.path.exists(path) and stat.S_ISSOCK(os.stat(path).st_mode))
check("socket is 0600, never world-connectable", mode(path) == 0o600)
check("parent dir mode untouched", mode(sockdir) == before)
check("no staging dir left behind", glob.glob(os.path.join(sockdir, ".private-*")) == [])
try:
    srv.remove_from_connection()
except Exception:
    pass
os.unlink(path)

# a bind failure must leave nothing at the real path, no staging dir, and the
# shared dir's mode unchanged
saved = m.PrivateSocketServer
def boom(*a, **k):
    raise RuntimeError("bind failed")
m.PrivateSocketServer = boom
raised = False
try:
    m._bind_private_socket(path)
except RuntimeError:
    raised = True
finally:
    m.PrivateSocketServer = saved
check("bind failure propagates", raised)
check("no socket left at the real path after a failed bind", not os.path.exists(path))
check("no staging dir left behind after a failed bind",
      glob.glob(os.path.join(sockdir, ".private-*")) == [])
check("parent dir mode untouched after a failed bind", mode(sockdir) == before)

# a stale socket at the path is replaced atomically
open(path, "w").close()
srv = m._bind_private_socket(path)
check("stale file at the path replaced by the socket", stat.S_ISSOCK(os.stat(path).st_mode))
check("replaced socket is 0600", mode(path) == 0o600)
try:
    srv.remove_from_connection()
except Exception:
    pass

print("test_systemd1_socket: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
