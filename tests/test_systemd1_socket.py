#!/usr/bin/env python3
# The bridge's private socket (/run/systemd/private) is bound by dbus at mode
# 0777 and narrowed to 0600 only afterward; a connection in that window has
# sender=None, which authorize() trusts as root. _bind_private_socket must close
# that window by locking the parent dir while the socket is created.
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


# normal creation: socket ends 0600, parent dir mode preserved
root = tempfile.mkdtemp()
sockdir = os.path.join(root, "systemd")
os.mkdir(sockdir, 0o755)
path = os.path.join(sockdir, "private")
srv = m._bind_private_socket(path)
check("socket created", os.path.exists(path) and stat.S_ISSOCK(os.stat(path).st_mode))
check("socket is 0600, not world-connectable", mode(path) == 0o600)
check("parent dir restored to 0755", mode(sockdir) == 0o755)
try:
    srv.remove_from_connection()
except Exception:
    pass
os.unlink(path)

# the lock is released even when the bind fails after the dir is locked, so a
# failure can't leave the real /run/systemd stuck at 0700
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
check("parent dir restored after a failed bind", mode(sockdir) == 0o755)

print("test_systemd1_socket: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
