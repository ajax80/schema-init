#!/usr/bin/env python3
"""systemctl reload dbus.service: schema-systemd1 calls ReloadConfig on the
system bus instead of SIGHUPing the broker, which an upgraded-but-not-rebooted
box would still be running in a version that dies on SIGHUP."""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("systemd1", os.path.join(HERE, "..", "scripts", "schema-systemd1.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

results = []
def check(name, ok):
    results.append(bool(ok)); print(("  PASS  " if ok else "  FAIL  ") + name)


class Unit:
    def __init__(self, pid, can_reload):
        self.pid, self.can_reload = pid, can_reload


class Mgr:
    def __init__(self):
        self.units = {"dbus.service": Unit(428, False), "foo.service": Unit(99, False)}
        self.signals = []
    def poll_and_update(self):
        pass
    # who may call is test_systemd1_authz's concern; this file tests what runs
    def _gated(self, message, action, proceed, error):
        proceed()
    _reload_unit = m.Systemd1Manager._reload_unit
    def _trigger_job_signals(self, name):
        self.signals.append(name)
        return "/job/1"


calls, kills = [], []
def fake_reload(done, fail):
    calls.append("ReloadConfig"); done()
m._reload_dbus = fake_reload
m.os.kill = lambda pid, sig: kills.append((pid, sig))
def call(meth, mgr, unit):
    out = []
    meth(mgr, unit, "replace", reply=out.append, error=lambda e: out.append(e))
    return out[0] if out else None
reload_unit = lambda mgr, unit, mode: call(m.Systemd1Manager.ReloadUnit, mgr, unit)

mgr = Mgr()
r = reload_unit(mgr, "dbus.service", "replace")
check("dbus.service reload calls ReloadConfig", calls == ["ReloadConfig"] and r == "/job/1")
check("dbus.service reload never signals the broker", kills == [])


def boom(done, fail):
    fail(m.dbus.exceptions.DBusException("reload failed: rules 412 -> 3",
                                         name="org.freedesktop.DBus.Error.Failed"))
m._reload_dbus = boom
r = reload_unit(Mgr(), "dbus.service", "replace")
check("failing ReloadConfig reaches the caller as an error",
      isinstance(r, m.dbus.exceptions.DBusException) and "rules 412 -> 3" in str(r))

try:
    reload_unit(Mgr(), "foo.service", "replace")
    check("other units without reload=1 still refuse", False)
except m.dbus.exceptions.DBusException as e:
    check("other units without reload=1 still refuse", "does not support reload" in str(e) and kills == [])

ctl = []
m.schema_ctl = lambda action, name: ctl.append((action, name))
m._reload_dbus = fake_reload
for meth in ("ReloadOrRestartUnit", "ReloadOrTryRestartUnit"):
    for unit in ("dbus.service", "dbus-broker.service"):
        calls.clear(); ctl.clear()
        mg = Mgr()
        mg._is_active = lambda n: True
        call(getattr(m.Systemd1Manager, meth), mg, unit)
        check("%s(%s) reloads, never restarts" % (meth, unit), calls == ["ReloadConfig"] and ctl == [])
calls.clear()
reload_unit(Mgr(), "dbus-broker.service", "replace")
check("ReloadUnit(dbus-broker.service) calls ReloadConfig", calls == ["ReloadConfig"] and kills == [])

u = m.Systemd1Unit.__new__(m.Systemd1Unit)
u.short_name = "dbus"
check("dbus.service reports CanReload", m.Systemd1Unit._read_can_reload(u) is True)

class _M: recently_stopped = set()
v = m.Systemd1Unit.__new__(m.Systemd1Unit)
v.name, v.short_name, v.manager = "loopy.service", "loopy", _M()
v.update_state({"pid": 0, "state": "EXCISED", "restarts": 9, "circuit_broken": True})
check("breaker-excised unit: failed + Result=start-limit-hit",
      v.active_state == "failed" and v.result == "start-limit-hit")
v.update_state({"pid": 7, "state": "SETTLED", "restarts": 0, "circuit_broken": False})
check("after reset: Result=success", v.result == "success")
for reason, want in (("start-timeout", "timeout"), ("main-pid-timeout", "timeout"),
                     ("exit-code", "exit-code"), ("signal", "signal"), ("core-dump", "core-dump"),
                     ("readiness-lost", "protocol"), ("no-cgroup", "resources"),
                     ("listen-fail", "resources"), ("fuse", "resources"), ("probe-failed", "resources"),
                     ("none", "success")):
    v.update_state({"pid": 0, "state": "EXCISED", "restarts": 0, "circuit_broken": False, "excise_reason": reason})
    check("excise_reason=%s: Result=%s" % (reason, want), v.result == want)
v.update_state({"pid": 0, "state": "EXCISED", "restarts": 0, "circuit_broken": True})
check("circuit_broken without excise_reason (older PID 1): start-limit-hit", v.result == "start-limit-hit")
v.update_state({"pid": 0, "state": "PERFECT", "restarts": 0, "circuit_broken": False, "excise_reason": "exit-code"})
check("a reason outside EXCISED is not a failure: Result=success", v.result == "success")
v.update_state({"pid": 0, "state": "EXCISED", "restarts": 0, "circuit_broken": False, "excise_reason": "bind"})
check("bind-held: inactive/dead, Result=success (systemd BindsTo= stop)",
      v.active_state == "inactive" and v.sub_state == "dead" and v.result == "success")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
