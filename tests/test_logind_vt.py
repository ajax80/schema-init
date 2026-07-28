#!/usr/bin/env python3
"""VT-switch session handoff test for schema-logind.

Runs schema-logind.py against a private bus and drives it with a fake
compositor: TakeControl, TakeDevice, then a simulated active-VT change.
Asserts the session-device handoff signals a real compositor needs.

Never touches the live logind, the real VTs, or the display. The active-VT
source is read from $SCHEMA_LOGIND_ACTIVE_VT so the test can point it at a
temp file; production still defaults to /sys/class/tty/tty0/active.

  ./tests/test_logind_vt.py        exit 0 all pass, 1 any fail
"""
import os
import signal
import subprocess
import sys
import tempfile
import time

import dbus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGIND = os.path.join(REPO, 'scripts', 'schema-logind.py')
BUS_NAME = 'org.freedesktop.login1'
SESSION_PATH = '/org/freedesktop/login1/session/_31'
SESSION_IFACE = 'org.freedesktop.login1.Session'
TEST_MAJOR, TEST_MINOR = 1, 3          # /dev/null — safe to open O_RDWR
SIGNAL_TIMEOUT = 5.0

results = []


def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def wait_for_signal(bus, member, timeout=SIGNAL_TIMEOUT):
    """Block until `member` arrives on the session object. Returns args or None."""
    got = []
    loop = GLib.MainLoop()

    def on_signal(*args):
        got.append(args)
        loop.quit()

    match = bus.add_signal_receiver(
        on_signal, signal_name=member, dbus_interface=SESSION_IFACE, path=SESSION_PATH)
    expired = []

    def on_timeout():
        expired.append(True)
        loop.quit()
        return False

    tid = GLib.timeout_add(int(timeout * 1000), on_timeout)
    loop.run()
    if not expired:
        GLib.source_remove(tid)
    match.remove()
    return got[0] if got else None


def main():
    DBusGMainLoop(set_as_default=True)

    vtfile = tempfile.NamedTemporaryFile('w', suffix='.activevt', delete=False)
    vtfile.write('tty1\n')
    vtfile.close()

    daemon = subprocess.Popen(
        ['dbus-daemon', '--session', '--print-address', '--nofork'],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    addr = daemon.stdout.readline().strip()
    if not addr:
        print("could not start a private bus", file=sys.stderr)
        return 1

    env = dict(os.environ)
    env['DBUS_SYSTEM_BUS_ADDRESS'] = addr
    env['SCHEMA_LOGIND_ACTIVE_VT'] = vtfile.name
    env['SCHEMA_LOGIND_VTNR'] = '1'

    stub = subprocess.Popen([sys.executable, LOGIND], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    bus = dbus.bus.BusConnection(addr)

    try:
        for _ in range(50):
            if stub.poll() is not None:
                print("schema-logind exited early:\n" + stub.stdout.read(), file=sys.stderr)
                return 1
            try:
                if bus.name_has_owner(BUS_NAME):
                    break
            except dbus.DBusException:
                pass
            time.sleep(0.1)
        else:
            print(f"{BUS_NAME} never appeared on the private bus", file=sys.stderr)
            return 1

        session = bus.get_object(BUS_NAME, SESSION_PATH)
        iface = dbus.Interface(session, SESSION_IFACE)
        props = dbus.Interface(session, 'org.freedesktop.DBus.Properties')

        print("\n-- fake compositor acquires the session --")
        try:
            iface.TakeControl(False)
            check('TakeControl succeeds', True)
        except dbus.DBusException as e:
            check('TakeControl succeeds', False, e.get_dbus_name())
            return 1

        try:
            fd, paused = iface.TakeDevice(TEST_MAJOR, TEST_MINOR)
            check('TakeDevice returns an fd', fd is not None, f"paused={bool(paused)}")
        except dbus.DBusException as e:
            check('TakeDevice returns an fd', False, e.get_dbus_name())
            return 1

        all_props = props.GetAll(SESSION_IFACE)
        check('session exposes VTNr', 'VTNr' in all_props,
              'present' if 'VTNr' in all_props else 'missing — logind cannot tell which VT it owns')
        check('session starts Active', bool(all_props.get('Active', False)))

        print("\n-- active VT moves away (tty1 -> tty2) --")
        with open(vtfile.name, 'w') as f:
            f.write('tty2\n')

        args = wait_for_signal(bus, 'PauseDevice')
        check('PauseDevice signal is emitted', args is not None,
              f"args={tuple(args)}" if args else f"no signal within {SIGNAL_TIMEOUT}s")
        if args:
            check('PauseDevice names the taken device',
                  (int(args[0]), int(args[1])) == (TEST_MAJOR, TEST_MINOR),
                  f"{int(args[0])}:{int(args[1])}")
            check("PauseDevice type is 'gone' or 'force'", str(args[2]) in ('gone', 'force'),
                  str(args[2]))

        check('session goes inactive', not bool(props.Get(SESSION_IFACE, 'Active')))

        print("\n-- active VT comes back (tty2 -> tty1) --")
        with open(vtfile.name, 'w') as f:
            f.write('tty1\n')

        args = wait_for_signal(bus, 'ResumeDevice')
        check('ResumeDevice signal is emitted', args is not None,
              f"args={tuple(args)}" if args else f"no signal within {SIGNAL_TIMEOUT}s")
        check('session goes active again', bool(props.Get(SESSION_IFACE, 'Active')))

        print("\n-- SIGHUP re-exec keeps the session alive --")
        # A kill+respawn would lose the device fds, and a fresh open() cannot
        # drop the master a compositor holds. The bridge must exec ITSELF: same
        # pid (the kernel's VT_PROCESS owner), same fds, new code.
        old_pid = stub.pid
        old_owner = bus.get_name_owner(BUS_NAME)
        os.kill(stub.pid, signal.SIGHUP)

        new_owner = None
        for _ in range(50):
            time.sleep(0.1)
            if stub.poll() is not None:
                break
            try:
                owner = bus.get_name_owner(BUS_NAME)
            except dbus.DBusException:
                continue
            if owner != old_owner:
                new_owner = owner
                break

        if not check('survives SIGHUP', stub.poll() is None,
                     'exited' if stub.poll() is not None else 'still running'):
            return 1
        check('pid is unchanged (exec, not restart)', stub.pid == old_pid,
              f"{old_pid} -> {stub.pid}")
        check('reconnects to the bus under a new connection', new_owner is not None,
              f"{old_owner} -> {new_owner}" if new_owner else 'name owner never changed')

        session = bus.get_object(BUS_NAME, SESSION_PATH)
        iface = dbus.Interface(session, SESSION_IFACE)
        props = dbus.Interface(session, 'org.freedesktop.DBus.Properties')

        all_props = props.GetAll(SESSION_IFACE)
        check('VTNr survives the handoff', str(all_props.get('VTNr', '')) == '1',
              str(all_props.get('VTNr', 'missing')))
        check('Active survives the handoff', bool(all_props.get('Active', False)))

        print("\n-- the adopted device still pauses (tty1 -> tty2) --")
        # The load-bearing assertion: PauseDevice can only name this device if
        # the fd travelled through execv. A cold-started bridge has an empty
        # device map and stays silent here.
        with open(vtfile.name, 'w') as f:
            f.write('tty2\n')

        args = wait_for_signal(bus, 'PauseDevice')
        check('adopted device still emits PauseDevice', args is not None,
              f"args={tuple(args)}" if args else 'device map was lost across exec')
        if args:
            check('adopted device keeps its identity',
                  (int(args[0]), int(args[1])) == (TEST_MAJOR, TEST_MINOR),
                  f"{int(args[0])}:{int(args[1])}")

    finally:
        for p in (stub, daemon):
            try:
                p.terminate()
                p.wait(timeout=5)
            except Exception:
                p.kill()
        os.unlink(vtfile.name)

    failed = [n for n, ok, _ in results if not ok]
    print(f"\n>> {len(results) - len(failed)}/{len(results)} passed")
    if failed:
        print(">> RESULT: FAIL — " + ', '.join(failed))
        return 1
    print(">> RESULT: PASS")
    return 0


if __name__ == '__main__':
    sys.exit(main())
