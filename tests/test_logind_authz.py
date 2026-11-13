#!/usr/bin/env python3
# schema-logind runs as root and serves hostname1/timedate1/locale1 (bus policy
# wide open) plus login1 session Unlock/ReleaseDevice (reachable by anyone).
# Each mutator must consult the ownership gate before doing anything, or any
# local uid rewrites /etc or unlocks the screen. Same class as the #469 bridge.
import importlib.util
import os
import shutil
import sys
import tempfile
import types

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


# --- who the gate trusts (audit 2026-11-12, 1.3) -----------------------------
# Not get_active_uid(): it falls back to 1000 at the login screen and to min()
# with two users. authz_uid() reads the root-written session files and the VT.
def authz_case(sessions, run_user, vt):
    d = tempfile.mkdtemp(prefix="logind-authz-")
    try:
        for sub_ in ("sessions", "run-user", "linger"):
            os.mkdir(os.path.join(d, sub_))
        for sid, body in sessions.items():
            with open(os.path.join(d, "sessions", sid), "w") as f:
                f.write(body)
        for u in run_user:
            os.mkdir(os.path.join(d, "run-user", str(u)))
        with open(os.path.join(d, "vt"), "w") as f:
            f.write(vt + "\n")
        saved = (m.SESSIONS_DIR, m.RUN_USER_DIR, m.ACTIVE_VT_PATH, m.LINGER_DIR)
        m.SESSIONS_DIR, m.RUN_USER_DIR = d + "/sessions", d + "/run-user"
        m.ACTIVE_VT_PATH, m.LINGER_DIR = d + "/vt", d + "/linger"
        try:
            return m.authz_uid()
        finally:
            m.SESSIONS_DIR, m.RUN_USER_DIR, m.ACTIVE_VT_PATH, m.LINGER_DIR = saved
    finally:
        shutil.rmtree(d)


def sess(uid, vt, active):
    return "UID=%d\nVTNR=%d\nACTIVE=%d\nSTATE=%s\n" % (uid, vt, active, "active" if active else "online")


check("login screen, no session, no /run/user: nobody (was 1000)",
      authz_case({}, [], "tty1") is None)
check("no session files, one /run/user: that uid", authz_case({}, [1000], "tty1") == 1000)
check("no session files, two /run/user: nobody (was the lower uid)",
      authz_case({}, [1000, 1001], "tty1") is None)
check("two users: the one on the front VT (was the lower uid)",
      authz_case({"1": sess(1000, 1, 0), "2": sess(1001, 2, 1)}, [1000, 1001], "tty2") == 1001)
check("no session on the front VT: the single active one",
      authz_case({"1": sess(1000, 1, 1), "2": sess(1001, 2, 0)}, [1000, 1001], "tty5") == 1000)
check("no session on the front VT and two active: nobody",
      authz_case({"1": sess(1000, 1, 1), "2": sess(1001, 2, 1)}, [1000, 1001], "tty5") is None)
check("session files present, none active, none in front: nobody",
      authz_case({"1": sess(1000, 3, 0)}, [1000], "tty1") is None)


# --- the gate decisions ------------------------------------------------------
m.authz_uid = lambda: 1000

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

# --- device fds go to the session controller only (audit 2026-11-12, 1.4) ----
class Bus(Conn):
    def __init__(self, uid, alive=()):
        Conn.__init__(self, uid)
        self.alive = set(alive)
    def name_has_owner(self, name):
        return name in self.alive


def session(uid, controller=None, unknown=False, alive=()):
    S = m.Login1Session
    o = types.SimpleNamespace(_connection=Bus(uid, alive), controller=controller,
                              controller_unknown=unknown, devices={}, vtnr=1)
    o._setup_vt_mediation = o._teardown_vt_mediation = lambda: None
    o._require_controller = lambda snd, what: S._require_controller(o, snd, what)
    o._controller_alive = lambda: S._controller_alive(o)
    return o


def err(fn):
    try:
        fn()
    except dbus.exceptions.DBusException as e:
        return e.get_dbus_name().rsplit(".", 1)[-1]
    return None


S = m.Login1Session
o = session(1000)
check("TakeDevice before TakeControl: NotInControl",
      err(lambda: S.TakeDevice(o, 13, 64, sender=":1.5")) == "NotInControl")
S.TakeControl(o, False, sender=":1.5")
o._connection.alive.add(":1.5")
check("TakeControl records the caller", o.controller == ":1.5")
check("same uid, other process: NotInControl",
      err(lambda: S.TakeDevice(o, 13, 64, sender=":1.9")) == "NotInControl")
check("the controller gets past the gate (to the open)",
      err(lambda: S.TakeDevice(o, 0, 0, sender=":1.5")) == "FailedToOpenDevice")
check("a second TakeControl while the controller lives: refused",
      err(lambda: S.TakeControl(o, False, sender=":1.9")) == "DeviceIsTaken")
check("ReleaseDevice from a non-controller: NotInControl",
      err(lambda: S.ReleaseDevice(o, 13, 64, sender=":1.9")) == "NotInControl")
o._connection.alive.clear()
S.TakeControl(o, False, sender=":1.9")
check("a crashed controller's restart takes over", o.controller == ":1.9")
check("root needs no TakeControl",
      err(lambda: S.TakeDevice(session(0), 0, 0, sender=":1.2")) == "FailedToOpenDevice")
u = session(1000, unknown=True)
check("handoff from an older bridge: the first device caller is adopted",
      err(lambda: S.TakeDevice(u, 0, 0, sender=":1.7")) == "FailedToOpenDevice" and u.controller == ":1.7")
check("and only that one",
      err(lambda: S.TakeDevice(u, 0, 0, sender=":1.8")) == "NotInControl")

print("test_logind_authz: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
