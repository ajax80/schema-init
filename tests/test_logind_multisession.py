#!/usr/bin/env python3
"""Multi-session bus tests for schema-logind.

Runs the bridge against a private bus with a temp /run/systemd tree holding
two session files, and asserts the D-Bus surface actually reflects them:
two objects, two ListSessions rows, correct per-session properties, and
SessionNew/SessionRemoved as files appear and vanish.

This is the half the single-session stub could never do. Never touches the
live logind, the real VTs, or the real /run/systemd.

  ./tests/test_logind_multisession.py     exit 0 all pass, 1 any fail
"""
import os
import shutil
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
MANAGER_PATH = '/org/freedesktop/login1'
MANAGER_IFACE = 'org.freedesktop.login1.Manager'
SESSION_IFACE = 'org.freedesktop.login1.Session'
SIGNAL_TIMEOUT = 5.0
SETTLE = 1.0            # comfortably more than the 250 ms sync tick

results = []


def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def write_session(rundir, sid, **kv):
    body = ['# This is private data. Do not parse.']
    body += ['%s=%s' % (k, v) for k, v in kv.items()]
    path = os.path.join(rundir, 'sessions', str(sid))
    with open(path, 'w') as f:
        f.write('\n'.join(body) + '\n')


def wait_signal(bus, member, timeout=SIGNAL_TIMEOUT):
    loop = GLib.MainLoop()
    got = []

    def handler(*args):
        got.append(args)
        loop.quit()

    match = bus.add_signal_receiver(handler, signal_name=member,
                                    dbus_interface=MANAGER_IFACE)
    expired = []
    tid = GLib.timeout_add(int(timeout * 1000),
                           lambda: (expired.append(True), loop.quit())[0])
    loop.run()
    if not expired:
        GLib.source_remove(tid)
    match.remove()
    return got[0] if got else None


def props(bus, path):
    obj = bus.get_object(BUS_NAME, path)
    return obj.GetAll(SESSION_IFACE,
                      dbus_interface='org.freedesktop.DBus.Properties')


def main():
    DBusGMainLoop(set_as_default=True)

    rundir = tempfile.mkdtemp(prefix='schema-logind-multi-')
    for d in ('sessions', 'seats', 'users'):
        os.makedirs(os.path.join(rundir, d), exist_ok=True)

    vtfile = tempfile.NamedTemporaryFile('w', suffix='.activevt', delete=False)
    vtfile.write('tty1\n')
    vtfile.close()

    # Two sessions, two users, two VTs — the case the stub could not represent.
    write_session(rundir, 1, UID=1000, USER=os.environ.get('USER', 'root'),
                  SEAT='seat0', VTNR=1, TYPE='wayland', CLASS='user',
                  DESKTOP='KDE', STATE='active', ACTIVE=1, IS_DISPLAY=1,
                  REMOTE=0, LEADER=os.getpid())
    write_session(rundir, 2, UID=1000, USER=os.environ.get('USER', 'root'),
                  SEAT='seat0', VTNR=2, TYPE='tty', CLASS='user',
                  STATE='online', ACTIVE=0, IS_DISPLAY=0, REMOTE=0,
                  LEADER=os.getpid())

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
    env['SCHEMA_LOGIND_RUN_DIR'] = rundir
    env['SCHEMA_LOGIND_UDEV_DATA'] = rundir   # no cN:M files -> uaccess re-scan inert
    env['SCHEMA_LOGIND_DEV_DIR'] = rundir
    env.pop('SCHEMA_LOGIND_VTNR', None)

    stub = subprocess.Popen([sys.executable, LOGIND], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    bus = dbus.bus.BusConnection(addr)

    try:
        for _ in range(50):
            if stub.poll() is not None:
                print("schema-logind exited early:\n" + stub.stdout.read(),
                      file=sys.stderr)
                return 1
            try:
                if bus.name_has_owner(BUS_NAME):
                    break
            except dbus.DBusException:
                pass
            time.sleep(0.1)

        mgr = bus.get_object(BUS_NAME, MANAGER_PATH)

        print("-- two session files are two sessions --")
        sessions = mgr.ListSessions(dbus_interface=MANAGER_IFACE)
        ids = sorted(str(s[0]) for s in sessions)
        check('ListSessions returns both', ids == ['1', '2'], str(ids))
        paths = sorted(str(s[4]) for s in sessions)
        check('object paths are per-id',
              paths == ['/org/freedesktop/login1/session/_1',
                        '/org/freedesktop/login1/session/_2'], str(paths))

        print("\n-- each session carries its own properties --")
        p1 = props(bus, '/org/freedesktop/login1/session/_1')
        p2 = props(bus, '/org/freedesktop/login1/session/_2')
        check('session 1 Id', str(p1['Id']) == '1', str(p1['Id']))
        check('session 2 Id', str(p2['Id']) == '2', str(p2['Id']))
        check('distinct VTNr', (int(p1['VTNr']), int(p2['VTNr'])) == (1, 2),
              f"{p1['VTNr']} {p2['VTNr']}")
        check('distinct Type', (str(p1['Type']), str(p2['Type']))
              == ('wayland', 'tty'), f"{p1['Type']} {p2['Type']}")
        check('Active follows the file',
              (bool(p1['Active']), bool(p2['Active'])) == (True, False),
              f"{p1['Active']} {p2['Active']}")
        check('Scope is per-session',
              (str(p1['Scope']), str(p2['Scope']))
              == ('session-1.scope', 'session-2.scope'),
              f"{p1['Scope']} {p2['Scope']}")

        print("\n-- seat reflects the active session --")
        seat = bus.get_object(BUS_NAME, '/org/freedesktop/login1/seat/seat0')
        sp = seat.GetAll('org.freedesktop.login1.Seat',
                         dbus_interface='org.freedesktop.DBus.Properties')
        check('Seat.ActiveSession is session 1',
              str(sp['ActiveSession'][0]) == '1', str(sp['ActiveSession'][0]))
        check('Seat.Sessions lists both',
              sorted(str(s[0]) for s in sp['Sessions']) == ['1', '2'],
              str([str(s[0]) for s in sp['Sessions']]))

        print("\n-- derived state files are written --")
        seatf = os.path.join(rundir, 'seats', 'seat0')
        body = open(seatf).read() if os.path.exists(seatf) else ''
        check('seat file exists', bool(body), seatf)
        check('seat file names the active session', 'ACTIVE=1' in body,
              body.replace('\n', ' '))
        check('seat file lists both sessions', 'SESSIONS=1 2' in body,
              body.replace('\n', ' '))

        print("\n-- a new file becomes a session --")
        write_session(rundir, 3, UID=1000, USER=os.environ.get('USER', 'root'),
                      SEAT='seat0', VTNR=3, TYPE='tty', CLASS='user',
                      STATE='online', ACTIVE=0, LEADER=os.getpid())
        sig = wait_signal(bus, 'SessionNew')
        check('SessionNew is emitted', sig is not None,
              str(sig) if sig else 'no signal within %.1fs' % SIGNAL_TIMEOUT)
        if sig:
            check('SessionNew names the new id', str(sig[0]) == '3', str(sig[0]))
        ids = sorted(str(s[0]) for s in mgr.ListSessions(dbus_interface=MANAGER_IFACE))
        check('ListSessions now returns three', ids == ['1', '2', '3'], str(ids))

        print("\n-- a removed file removes the session --")
        os.unlink(os.path.join(rundir, 'sessions', '3'))
        sig = wait_signal(bus, 'SessionRemoved')
        check('SessionRemoved is emitted', sig is not None,
              str(sig) if sig else 'no signal within %.1fs' % SIGNAL_TIMEOUT)
        if sig:
            check('SessionRemoved names the gone id', str(sig[0]) == '3',
                  str(sig[0]))
        ids = sorted(str(s[0]) for s in mgr.ListSessions(dbus_interface=MANAGER_IFACE))
        check('ListSessions is back to two', ids == ['1', '2'], str(ids))

        print("\n-- GetSession resolves by id --")
        try:
            path = str(mgr.GetSession('2', dbus_interface=MANAGER_IFACE))
            check('GetSession(2) resolves',
                  path == '/org/freedesktop/login1/session/_2', path)
        except dbus.DBusException as e:
            check('GetSession(2) resolves', False, str(e))
        try:
            mgr.GetSession('nope', dbus_interface=MANAGER_IFACE)
            check('GetSession(unknown) raises', False, 'returned a path')
        except dbus.DBusException as e:
            check('GetSession(unknown) raises', 'NoSuchSession' in str(e),
                  e.get_dbus_name())

    finally:
        for p in (stub, daemon):
            try:
                p.terminate()
                p.wait(timeout=5)
            except Exception:
                p.kill()
        os.unlink(vtfile.name)
        shutil.rmtree(rundir, ignore_errors=True)

    failed = [r for r in results if not r[1]]
    print(f"\n>> {len(results) - len(failed)}/{len(results)} passed")
    print(">> RESULT: " + ("PASS" if not failed
                           else "FAIL — " + ", ".join(r[0] for r in failed)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
