#!/usr/bin/env python3
# The bridge runs schema-ctl as root, and the stock bus policy lets any user
# send it StartUnit and friends because real systemd checks polkit inside the
# daemon. Real bus, real bridge, fake polkit and fake schema-ctl.
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import dbus
    import dbus.bus
    import dbus.lowlevel
    import dbus.mainloop.glib
    import dbus.service
    from gi.repository import GLib
except ImportError as e:
    print("test_systemd1_authz: skipped (%s)" % e)
    sys.exit(0)
HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "..", "scripts", "schema-systemd1.py")
spec = importlib.util.spec_from_file_location("systemd1", BRIDGE)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

NAME = "org.freedesktop.systemd1"


# Re-run of this file as a bus peer: the bridge, or the fake polkit.
def run_bridge(addr):
    m.SCHEMA_CTL = wdir + "/schema-ctl"
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.bus.BusConnection(addr)
    keep = m.Systemd1Manager(bus)
    bus.request_name(NAME)
    GLib.MainLoop().run()
    del keep


class FakePolkit(dbus.service.Object):
    @dbus.service.method("org.freedesktop.PolicyKit1.Authority",
                         in_signature="(sa{sv})sa{ss}us", out_signature="(bba{ss})")
    def CheckAuthorization(self, subject, action, details, flags, cancel):
        with open(wdir + "/polkit-mode") as f:
            mode = f.read()
        with open(wdir + "/polkit.log", "a") as f:
            f.write(json.dumps({"action": str(action), "flags": int(flags),
                                "details": {str(k): str(v) for k, v in details.items()},
                                "subject": [str(subject[0]), str(subject[1]["name"])]}) + "\n")
        return (mode == "yes", mode == "challenge", {})


def run_polkit(addr):
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.bus.BusConnection(addr)
    keep = FakePolkit(bus, "/org/freedesktop/PolicyKit1/Authority")
    bus.request_name("org.freedesktop.PolicyKit1")
    GLib.MainLoop().run()
    del keep



if len(sys.argv) == 4:
    role, child_addr, wdir = sys.argv[1:]
    (run_bridge if role == "bridge" else run_polkit)(child_addr)
    sys.exit(0)

if not shutil.which("dbus-daemon"):
    print("test_systemd1_authz: skipped (no dbus-daemon)")
    sys.exit(0)
if os.getuid() == 0:
    print("test_systemd1_authz: skipped (needs a non-root uid)")
    sys.exit(0)

results = []
def check(name, ok):
    results.append(bool(ok)); print(("  PASS  " if ok else "  FAIL  ") + name)


PATH = "/org/freedesktop/systemd1"
MGR = "org.freedesktop.systemd1.Manager"
DENIED = "org.freedesktop.DBus.Error.AccessDenied"
CHALLENGE = "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired"
MANAGE = "org.freedesktop.systemd1.manage-units"
RELOAD = "org.freedesktop.systemd1.reload-daemon"
SETENV = "org.freedesktop.systemd1.set-environment"

work = tempfile.mkdtemp()
ctl_log = work + "/ctl.log"
mode_file = work + "/polkit-mode"
pk_log = work + "/polkit.log"
procs = []


# --- in-process: the decision itself, where the test can pick the caller's uid
class FakeBus:
    def __init__(self, uid):
        self.uid, self.polkit_calls = uid, 0
    def call_async(self, name, path, iface, member, sig, args, ok, err, **k):
        if member == "GetConnectionUnixUser":
            ok(self.uid)
        else:
            self.polkit_calls += 1


def decide(uid, sender):
    bus, got = FakeBus(uid), []
    m.authorize(bus, sender, False, MANAGE, {}, "StopUnit", lambda: got.append("run"), lambda e: got.append("deny"))
    return got, bus.polkit_calls


check("root passes without asking polkit", decide(0, ":1.5") == (["run"], 0))
check("private socket (no sender) passes without asking polkit", decide(1000, None) == (["run"], 0))
check("a user neither runs nor is denied until polkit answers", decide(1000, ":1.5") == ([], 1))


class UidFails(FakeBus):
    def call_async(self, name, path, iface, member, sig, args, ok, err, **k):
        err(dbus.DBusException("gone", name="org.freedesktop.DBus.Error.NameHasNoOwner"))


got = []
m.authorize(UidFails(0), ":1.5", False, MANAGE, {}, "StopUnit", lambda: got.append("run"), lambda e: got.append("deny"))
check("uid lookup failure is a refusal", got == ["deny"])

# A new mutating method that skips the gate is an unauthenticated root action.
OPEN = {"GetUnitFileState", "GetUnit", "LoadUnit", "ListUnits", "ListUnitsFiltered", "ListUnitsByNames",
        "ResetFailed", "ResetFailedUnit", "KillUnit", "GetDefaultTarget", "ListUnitFiles", "UnmaskUnitFiles",
        "PresetUnitFiles", "ListJobs", "CancelJob", "StartTransientUnit", "AttachProcessesToUnit",
        "EnqueueMarkedJobs", "Reexecute", "SetUnitProperties", "EnableUnitFiles", "DisableUnitFiles",
        "MaskUnitFiles", "Subscribe", "Unsubscribe"}
mgr_methods = {k: v for k, v in vars(m.Systemd1Manager).items()
               if getattr(v, "_dbus_is_method", False) and v._dbus_interface == "org.freedesktop.systemd1.Manager"}
unlisted = sorted(k for k, v in mgr_methods.items() if k not in OPEN and v._dbus_message_keyword != "message")
check("every Manager method is gated or knowingly open: %s" % unlisted, not unlisted)
check("no open-listed method is stale", OPEN <= set(mgr_methods))


# --- real bus -----------------------------------------------------------------
fake_ctl = work + "/schema-ctl"
with open(fake_ctl, "w") as f:
    f.write('#!/bin/sh\n'
            '[ "$1" = status ] && { cat %s/status.json; exit 0; }\n'
            'echo "$*" >> %s\n' % (work, ctl_log))
os.chmod(fake_ctl, 0o755)
with open(work + "/status.json", "w") as f:
    json.dump({"services": [{"name": "svc", "pid": os.getpid(), "state": "FUNDAMENTAL"}]}, f)

conf = work + "/bus.conf"
with open(conf, "w") as f:
    f.write('<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"'
            ' "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd"><busconfig>'
            '<type>session</type><listen>unix:path=%s/bus</listen><policy context="default">'
            '<allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/>'
            '<allow user="*"/></policy></busconfig>' % work)
procs.append(subprocess.Popen(["dbus-daemon", "--nofork", "--config-file", conf],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
ADDR = "unix:path=%s/bus" % work
for _ in range(50):
    if os.path.exists(work + "/bus"):
        break
    time.sleep(0.1)
client = dbus.bus.BusConnection(ADDR)


def spawn(role):
    procs.append(subprocess.Popen([sys.executable, "-u", os.path.abspath(__file__), role, ADDR, work],
                                  stdout=open(work + "/%s.out" % role, "w"), stderr=open(work + "/%s.err" % role, "w")))
    return procs[-1]


def wait_name(name):
    for _ in range(100):
        if client.name_has_owner(name):
            return True
        time.sleep(0.1)
    return False


def ctl_calls():
    try:
        with open(ctl_log) as f:
            return f.read().split("\n")[:-1]
    except FileNotFoundError:
        return []


def reset(mode):
    with open(mode_file, "w") as f:
        f.write(mode)
    for p in (ctl_log, pk_log):
        if os.path.exists(p):
            os.remove(p)


def call(member, sig, args, interactive=False):
    msg = dbus.lowlevel.MethodCallMessage(NAME, PATH, MGR, member)
    if args:
        msg.append(*args, signature=sig)
    if interactive:
        msg.set_allow_interactive_authorization(True)
    try:
        client.send_message_with_reply_and_block(msg, 10)
        return None
    except dbus.DBusException as e:
        return e.get_dbus_name()


def polkit_seen():
    with open(pk_log) as f:
        return json.loads(f.read().splitlines()[-1])


U = ("svc.service", "replace")
GATED = [
    ("StartUnit", "ss", U, MANAGE),
    ("StopUnit", "ss", U, MANAGE),
    ("RestartUnit", "ss", U, MANAGE),
    ("TryRestartUnit", "ss", U, MANAGE),
    ("ReloadUnit", "ss", U, MANAGE),
    ("ReloadOrRestartUnit", "ss", U, MANAGE),
    ("ReloadOrTryRestartUnit", "ss", U, MANAGE),
    ("Reload", "", (), RELOAD),
    ("SetEnvironment", "as", (["A=1"],), SETENV),
    ("UnsetEnvironment", "as", (["A"],), SETENV),
    ("UnsetAndSetEnvironment", "asas", (["B"], ["A=1"]), SETENV),
]


def env():
    props = dbus.Interface(client.get_object(NAME, PATH), "org.freedesktop.DBus.Properties")
    return [str(x) for x in props.Get(MGR, "Environment")]


def main():
    bridge = spawn("bridge")
    assert wait_name(NAME), "bridge never owned its name: " + open(work + "/bridge.err").read()

    reset("no")
    check("no polkit on the bus: StopUnit denied", call("StopUnit", "ss", U) == DENIED)
    check("no polkit on the bus: schema-ctl not run", ctl_calls() == [])

    spawn("polkit")
    assert wait_name("org.freedesktop.PolicyKit1"), open(work + "/polkit.err").read()

    for member, sig, args, action in GATED:
        reset("no")
        before = env()
        check("%s refused: AccessDenied" % member, call(member, sig, args) == DENIED)
        check("%s refused: nothing happened" % member, ctl_calls() == [] and env() == before)
        check("%s asks for %s" % (member, action.rsplit(".", 1)[1]), polkit_seen()["action"] == action)

    reset("challenge")
    check("challenge without the client's flag is InteractiveAuthorizationRequired",
          call("StopUnit", "ss", U) == CHALLENGE)
    check("challenge: schema-ctl not run", ctl_calls() == [])
    check("no client flag: polkit told not to prompt", polkit_seen()["flags"] == 0)
    reset("challenge")
    call("StopUnit", "ss", U, interactive=True)
    check("client flag: polkit allowed to prompt", polkit_seen()["flags"] == 1)
    check("subject is the caller's bus name",
          polkit_seen()["subject"] == ["system-bus-name", client.get_unique_name()])

    reset("yes")
    for member, verb in (("StartUnit", "start"), ("StopUnit", "stop"), ("RestartUnit", "restart"),
                         ("TryRestartUnit", "restart"), ("ReloadOrRestartUnit", "restart"),
                         ("ReloadOrTryRestartUnit", "restart")):
        reset("yes")
        check("%s authorized: replies" % member, call(member, "ss", U) is None)
        check("%s authorized: schema-ctl %s svc" % (member, verb), ctl_calls() == ["%s svc" % verb])
    reset("yes")
    check("Reload authorized: schema-ctl reload", call("Reload", "", ()) is None and ctl_calls() == ["reload"])
    reset("yes")
    check("SetEnvironment authorized: applied", call("SetEnvironment", "as", (["ZZ=1"],)) is None and "ZZ=1" in env())
    reset("yes")
    check("UnsetAndSetEnvironment authorized: applied",
          call("UnsetAndSetEnvironment", "asas", (["ZZ"], ["YY=2"])) is None
          and "YY=2" in env() and "ZZ=1" not in env())
    # the handler's own error must reach the caller once it runs after polkit
    check("ReloadUnit authorized: its own error still reaches the caller",
          call("ReloadUnit", "ss", ("nope.service", "replace")) == "org.freedesktop.systemd1.NoSuchUnit")

    # schema-ctl strips the suffix, so all of these reach sshd: a polkit rule on
    # sshd.service must see the same unit whichever spelling the caller picks.
    for spelled in ("sshd.service", "sshd", "sshd.target"):
        reset("no")
        call("StopUnit", "ss", (spelled, "replace"))
        check("polkit details for %s" % spelled,
              polkit_seen()["details"] == {"unit": "sshd.service", "verb": "stop"})
    for member, verb in (("StartUnit", "start"), ("RestartUnit", "restart"), ("TryRestartUnit", "try-restart"),
                         ("ReloadUnit", "reload"), ("ReloadOrRestartUnit", "reload-or-restart"),
                         ("ReloadOrTryRestartUnit", "reload-or-try-restart")):
        reset("no")
        call(member, "ss", U)
        check("polkit verb for %s" % member, polkit_seen()["details"].get("verb") == verb)
    reset("no")
    call("Reload", "", ())
    check("Reload names no unit", polkit_seen()["details"] == {})

    forged = "systemd1: StopUnit['crond.service', 'replace'] uid=0 -> allowed"
    for member, sig, args in (("ResetFailedUnit", "s", ("x\n" + forged,)),
                              ("KillUnit", "ssi", ("x\n" + forged, "all", 15)),
                              ("SetUnitProperties", "sba(sv)", ("x\n" + forged, True, [])),
                              ("EnableUnitFiles", "asbb", (["x\n" + forged], False, False)),
                              ("StartTransientUnit", "ssa(sv)a(sa(sv))", ("x\n" + forged, "fail", [], [])),
                              ("AttachProcessesToUnit", "ssau", ("x\n" + forged, "/y", [1])),
                              ("StopUnit", "ss", ("x\n" + forged, "replace"))):
        reset("no")
        call(member, sig, args)
    time.sleep(0.3)
    with open(work + "/bridge.out") as f:
        check("no caller name can forge a log line", forged + "\n" not in f.read())
    reset("no")
    call("SetEnvironment", "as", (["A=" + "x" * 50000],))
    time.sleep(0.3)
    with open(work + "/bridge.out") as f:
        check("a refused call's log line is bounded", max(len(l) for l in f) < 1000)

    reset("no")
    check("reads need no authorization", call("ListUnits", "", ()) is None)
    check("no-op SetUnitProperties stays open", call("SetUnitProperties", "sba(sv)", ("svc.service", True, [])) is None)
    check("bridge survived", bridge.poll() is None)


try:
    main()
finally:
    for p in procs:
        p.terminate()
    shutil.rmtree(work, ignore_errors=True)
print("test_systemd1_authz: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
