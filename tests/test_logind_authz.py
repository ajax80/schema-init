#!/usr/bin/env python3
# schema-logind runs as root and serves hostname1/timedate1/locale1 (bus policy
# wide open) plus login1 session Unlock/ReleaseDevice (reachable by anyone).
# Each mutator must consult the ownership gate before doing anything, or any
# local uid rewrites /etc or unlocks the screen. Same class as the #469 bridge.
import importlib.util
import os
import sys

try:
    import dbus
except ImportError as e:
    print("test_logind_authz: skipped (%s)" % e)
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "slogind", os.path.join(HERE, "..", "scripts", "schema-logind.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

results = []
def check(name, ok):
    results.append(bool(ok)); print(("  PASS  " if ok else "  FAIL  ") + name)


class Conn:
    def __init__(self, uid):
        self.uid = uid
    def get_unix_user(self, sender):
        if self.uid is None:
            raise dbus.exceptions.DBusException("no sender")
        return self.uid


def denied(fn):
    try:
        fn()
    except dbus.exceptions.DBusException as e:
        return e.get_dbus_name().endswith("AccessDenied")
    return False


# --- the gate decisions ------------------------------------------------------
m.get_active_uid = lambda: 1000

check("authz_check: active-session uid passes",
      m.authz_check(Conn(1000), ":1", "X") is None)
check("authz_check: root passes", m.authz_check(Conn(0), ":1", "X") is None)
check("authz_check: a stranger is denied", denied(lambda: m.authz_check(Conn(65534), ":1", "X")))
check("authz_check: unresolved sender is denied (fail closed)",
      denied(lambda: m.authz_check(Conn(None), ":1", "X")))
check("authz_check: no sender (private peer) is denied",
      denied(lambda: m.authz_check(Conn(0), None, "X")))


# --- every mutator consults the gate before its body ------------------------
class Reached(Exception):
    pass


seen = []
def recording_gate(gate_name):
    def g(connection, sender, what):
        seen.append((gate_name, what, sender))
        raise Reached
    return g


# (class, method, extra positional args after self; sender passed by keyword)
GATED = [
    (m.Hostname1, "SetHostname", ("h", False), "authz_check"),
    (m.Hostname1, "SetStaticHostname", ("h", False), "authz_check"),
    (m.Hostname1, "SetPrettyHostname", ("h", False), "authz_check"),
    (m.Hostname1, "SetIconName", ("computer", False), "authz_check"),
    (m.Hostname1, "SetChassis", ("desktop", False), "authz_check"),
    (m.Hostname1, "SetDeployment", ("dev", False), "authz_check"),
    (m.Hostname1, "SetLocation", ("here", False), "authz_check"),
    (m.Timedate1, "SetTimezone", ("UTC", False), "authz_check"),
    (m.Locale1, "SetLocale", (["LANG=C"], False), "authz_check"),
    (m.Locale1, "SetVConsoleKeyboard", ("us", "", False, False), "authz_check"),
    (m.Locale1, "SetX11Keyboard", ("us", "", "", "", False, False), "authz_check"),
    (m.Login1Session, "Unlock", (), "authz_check"),
    (m.Login1Session, "ReleaseDevice", (0, 0), "authz_check"),
    (m.Login1Manager, "UnlockSession", ("1",), "authz_check"),
    (m.Login1Manager, "UnlockSessions", (), "authz_check"),
    (m.Login1Session, "Activate", (), "authz_check"),
    (m.Login1Session, "ReleaseControl", (), "authz_check"),
    (m.Login1Session, "PauseDeviceComplete", (0, 0), "authz_check"),
    (m.Login1Manager, "ReleaseSession", ("1",), "authz_check"),
]

saved_check = m.authz_check
m.authz_check = recording_gate("authz_check")
try:
    for cls, name, args, gate in GATED:
        meth = getattr(cls, name)
        self = object.__new__(cls)
        self._connection = Conn(65534)
        seen.clear()
        try:
            meth(self, *args, sender=":attacker")
            reached = False
        except Reached:
            reached = True
        except TypeError as e:
            check("%s.%s takes a sender and guards first" % (cls.__name__, name), False)
            print("        " + str(e))
            continue
        ok = reached and seen == [(gate, name, ":attacker")]
        check("%s.%s consults %s before its body" % (cls.__name__, name, gate), ok)
        if not ok:
            print("        seen=%r reached=%r" % (seen, reached))
finally:
    m.authz_check = saved_check

print("test_logind_authz: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
