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
    def _trigger_job_signals(self, name):
        self.signals.append(name)
        return "/job/1"


calls, kills = [], []
m._reload_dbus = lambda: calls.append("ReloadConfig")
m.os.kill = lambda pid, sig: kills.append((pid, sig))
reload_unit = m.Systemd1Manager.ReloadUnit

mgr = Mgr()
r = reload_unit(mgr, "dbus.service", "replace")
check("dbus.service reload calls ReloadConfig", calls == ["ReloadConfig"] and r == "/job/1")
check("dbus.service reload never signals the broker", kills == [])


def boom():
    raise m.dbus.exceptions.DBusException("reload failed: rules 412 -> 3",
                                          name="org.freedesktop.DBus.Error.Failed")
m._reload_dbus = boom
try:
    reload_unit(Mgr(), "dbus.service", "replace")
    check("failing ReloadConfig raises", False)
except m.dbus.exceptions.DBusException as e:
    check("failing ReloadConfig raises", "rules 412 -> 3" in str(e))

try:
    reload_unit(Mgr(), "foo.service", "replace")
    check("other units without reload=1 still refuse", False)
except m.dbus.exceptions.DBusException as e:
    check("other units without reload=1 still refuse", "does not support reload" in str(e) and kills == [])

u = m.Systemd1Unit.__new__(m.Systemd1Unit)
u.short_name = "dbus"
check("dbus.service reports CanReload", m.Systemd1Unit._read_can_reload(u) is True)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
