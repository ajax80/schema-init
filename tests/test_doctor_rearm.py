#!/usr/bin/env python3
"""RearmVtMediation hook test — private bus, drive schema-logind, assert the
method exists and returns cleanly. Mirrors tests/test_logind_vt.py setup."""
import os, sys, subprocess, tempfile, time

import dbus
from dbus.mainloop.glib import DBusGMainLoop

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGIND = os.path.join(REPO, "scripts", "schema-logind.py")
BUS_NAME = "org.freedesktop.login1"

results = []
def check(n, ok, detail=''): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}" + (f"  — {detail}" if detail else ''))

def main():
    DBusGMainLoop(set_as_default=True)
    vtfile = tempfile.NamedTemporaryFile("w", suffix=".activevt", delete=False)
    vtfile.write("tty1\n"); vtfile.close()

    daemon = subprocess.Popen(
        ["dbus-daemon", "--session", "--print-address", "--nofork"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    addr = daemon.stdout.readline().strip()
    if not addr:
        print("could not start a private bus", file=sys.stderr); return 1
    rundir = tempfile.mkdtemp(prefix="schema-logind-rearm-")

    env = dict(os.environ)
    env["DBUS_SYSTEM_BUS_ADDRESS"] = addr
    env["SCHEMA_LOGIND_ACTIVE_VT"] = vtfile.name
    env["SCHEMA_LOGIND_VTNR"] = "1"
    env["SCHEMA_LOGIND_RUN_DIR"] = rundir

    stub = subprocess.Popen([sys.executable, LOGIND], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    bus = dbus.bus.BusConnection(addr)
    try:
        for _ in range(50):
            if stub.poll() is not None:
                print("schema-logind exited early:\n" + stub.stdout.read(), file=sys.stderr); return 1
            try:
                if bus.name_has_owner(BUS_NAME):
                    break
            except dbus.DBusException:
                pass
            time.sleep(0.1)
        else:
            print("schema-logind never claimed the bus", file=sys.stderr); return 1

        obj = bus.get_object(BUS_NAME, "/org/freedesktop/login1")
        mgr = dbus.Interface(obj, "org.schema.logind1.Manager")
        err = str(mgr.RearmVtMediation())
        check("RearmVtMediation callable", True)
        check("returns no error", err == "", err)
    except Exception as e:
        check("RearmVtMediation callable", False, str(e))
    finally:
        stub.terminate(); daemon.terminate()

    print("PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1

if __name__ == "__main__":
    sys.exit(main())
