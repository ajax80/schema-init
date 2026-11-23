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


def sess(uid, vt, active, extra=""):
    return "UID=%d\nVTNR=%d\nACTIVE=%d\nSTATE=%s\n%s" % (uid, vt, active, "active" if active else "online", extra)


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
check("a remote session marked active is not the console",
      authz_case({"1": sess(1001, 0, 1, "REMOTE=1\n")}, [1001], "tty1") is None)


def linger_case(run_user, lingering):
    d = tempfile.mkdtemp(prefix="logind-linger-")
    saved = m.linger_uids
    m.linger_uids = lambda: set(lingering)
    try:
        return authz_case({}, run_user, "tty1")
    finally:
        m.linger_uids = saved
        shutil.rmtree(d)


check("no session files, the one user lingers: still that user",
      linger_case([1000], [1000]) == 1000)
check("no session files, a second user lingers: nobody (fail closed)",
      linger_case([1000, 1001], [1001]) is None)


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
    def g(connection, sender, what, owner=None):
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
    # review 2026-11-22: with login1 reachable by any uid (see own_extra_names),
    # locking someone else's screen or lying about its lock/idle state is open
    (m.Login1Session, "Lock", (), "authz_check"),
    (m.Login1Session, "SetLockedHint", (True,), "authz_check"),
    (m.Login1Session, "SetIdleHint", (True,), "authz_check"),
    (m.Login1Manager, "LockSession", ("1",), "authz_check"),
    (m.Login1Manager, "LockSessions", (), "authz_check"),
]

saved_check = m.authz_check
m.authz_check = recording_gate("authz_check")
try:
    for cls, name, args, gate in GATED:
        meth = getattr(cls, name)
        self = object.__new__(cls)
        self._connection = Conn(65534)
        # Manager lock relays read the target's owner before gating (read-only)
        self._lookup = lambda sid: types.SimpleNamespace(_owner=lambda: None)
        self.registry = types.SimpleNamespace(sessions={})
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


def session(uid, controller=None, unknown=False, alive=(), owner=1000, compositor=True):
    S = m.Login1Session
    rec = m.SessionRecord("1", {"UID": str(owner)} if owner is not None else None,
                          synthesised=owner is None)
    o = types.SimpleNamespace(_connection=Bus(uid, alive), controller=controller,
                              controller_unknown=unknown, devices={}, vtnr=1, record=rec)
    o._setup_vt_mediation = o._teardown_vt_mediation = lambda: None
    o._require_controller = lambda snd, what: S._require_controller(o, snd, what)
    o._controller_alive = lambda: S._controller_alive(o)
    o._owner = lambda: S._owner(o)
    o._is_compositor = lambda snd: compositor
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
check("re-exec lost the controller: the compositor is adopted",
      err(lambda: S.TakeDevice(u, 0, 0, sender=":1.7")) == "FailedToOpenDevice" and u.controller == ":1.7")
check("and only that one",
      err(lambda: S.TakeDevice(u, 0, 0, sender=":1.8")) == "NotInControl")
k = session(1000, unknown=True, compositor=False)
check("re-exec lost the controller: a non-compositor is not adopted",
      err(lambda: S.TakeDevice(k, 0, 0, sender=":1.6")) == "NotInControl" and k.controller is None)
check("ReleaseControl with no controller: refused to non-root",
      err(lambda: S.ReleaseControl(session(1000, unknown=True), sender=":1.6")) == "NotInControl")

m.authz_uid = lambda: 1000      # user 1000 in front; 1001's session is behind
b = session(1001, owner=1001)
check("a background session's own compositor may TakeControl",
      err(lambda: S.TakeControl(b, False, sender=":2.1")) is None and b.controller == ":2.1")
check("another uid may not",
      err(lambda: S.TakeControl(session(1002, owner=1001), False, sender=":2.2")) == "AccessDenied")
m.authz_uid = lambda: None      # login screen
check("a stand-in session's guessed uid gives no ownership",
      err(lambda: S.TakeControl(session(1000, owner=None), False, sender=":2.3")) == "AccessDenied")


class Reg:
    def __init__(self, obj):
        self.sessions = {"1": obj}
    def sync(self):
        pass


mgr = m.Login1Manager.__new__(m.Login1Manager)
mgr._connection = Conn(1000)
mgr.registry = Reg(session(1000, owner=None))
check("ReleaseSession of a stand-in session at the login screen: denied",
      denied(lambda: m.Login1Manager.ReleaseSession(mgr, "1", sender=":2.4")))

# --- Manager lock relays reach the session (review of #572) ------------------
# The relay called Session.Lock()/Unlock() with no sender, so their own gate saw
# an unresolved caller and refused even root: loginctl lock-session did nothing.
class LockSess:
    def __init__(self, owner):
        self.emitted = []
        self.owner = owner
    def _owner(self):
        return self.owner
    def _emit_bare_signal(self, member):
        self.emitted.append(member)

def relay(uid, method, *args, owner=1000, front=1000):
    saved_front = m.authz_uid
    m.authz_uid = lambda: front
    mgr = m.Login1Manager.__new__(m.Login1Manager)
    mgr._connection = Conn(uid)
    s1 = LockSess(owner)
    mgr.registry = types.SimpleNamespace(sessions={"1": s1})
    mgr._lookup = lambda sid: s1
    try:
        e = err(lambda: getattr(m.Login1Manager, method)(mgr, *args, sender=":3.1"))
    finally:
        m.authz_uid = saved_front
    return e, s1.emitted

check("LockSession from root emits Lock", relay(0, "LockSession", "1") == (None, ["Lock"]))
check("LockSessions from the active user emits Lock", relay(1000, "LockSessions") == (None, ["Lock"]))
check("UnlockSession from root emits Unlock", relay(0, "UnlockSession", "1") == (None, ["Unlock"]))
check("UnlockSessions from root emits Unlock", relay(0, "UnlockSessions") == (None, ["Unlock"]))
check("LockSession of a background session by its owner: allowed",
      relay(1001, "LockSession", "1", owner=1001, front=1000) == (None, ["Lock"]))
check("LockSession of someone else's session: denied, nothing emitted",
      relay(1002, "LockSession", "1", owner=1001, front=1000) == ("AccessDenied", []))
check("UnlockSession of someone else's session: denied, nothing emitted",
      relay(1002, "UnlockSession", "1", owner=1001, front=1000) == ("AccessDenied", []))

# --- CreateSession is pam_systemd's, which runs as root (review 2026-11-22) ---
# A non-root caller named any pid and schema-logind wrote it into a session
# scope's cgroup.procs as root — PID 1 included.
def create(uid, vtnr=0, tty=""):
    mgr = m.Login1Manager.__new__(m.Login1Manager)
    mgr._connection = Conn(uid)
    return err(lambda: m.Login1Manager.CreateSession(
        mgr, 1000, 1, "login", "tty", "user", "", "seat0", vtnr, tty, "", False, "", "", [],
        sender=":2.9"))

check("CreateSession from uid 1000: AccessDenied", create(1000) == "AccessDenied")
check("CreateSession from an unresolved sender: AccessDenied", create(None) == "AccessDenied")
check("CreateSession from root passes the gate (no VT: SessionBusy)", create(0) == "SessionBusy")


# --- each extra name on its own connection (review 2026-11-22) ---------------
# send_destination matches any name the receiving connection owns, so the
# blanket allows that hostname1/timedate1/locale1/ConsoleKit ship covered every
# login1 method while all five shared one connection.
class FakeBus:
    made = []
    def __init__(self):
        self.names = []
        FakeBus.made.append(self)
    def request_name(self, name, flags=0):
        self.names.append(name)

saved = {k: getattr(m, k) for k in ("Hostname1", "Timedate1", "Locale1", "ConsoleKitSession", "ConsoleKitManager")}
for k in saved:
    setattr(m, k, lambda bus, *a, _k=k: (_k, bus))
try:
    FakeBus.made = []
    held = m.own_extra_names(FakeBus, 1000)
    owned = [b.names for b in FakeBus.made]
    check("own_extra_names: one connection per name",
          sorted(n for ns in owned for n in ns) == sorted(["org.freedesktop.hostname1", "org.freedesktop.timedate1",
                                                          "org.freedesktop.locale1", "org.freedesktop.ConsoleKit"])
          and all(len(ns) == 1 for ns in owned))
    check("own_extra_names: never claims login1", all("org.freedesktop.login1" not in ns for ns in owned))
    check("own_extra_names: ConsoleKit's two objects share its connection",
          sum(1 for o in held if o[0].startswith("ConsoleKit")) == 2
          and len({id(o[1]) for o in held if o[0].startswith("ConsoleKit")}) == 1)
finally:
    for k, v in saved.items():
        setattr(m, k, v)

print("test_logind_authz: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
