#!/usr/bin/env python3
"""End-to-end test of session allocation against the live bridge.

Drives the real scripts/schema-session-register and schema-session-unregister
against a real schema-logind on a private bus, with SCHEMA_LOGIND_RUN_DIR and
SCHEMA_CGROUP_ROOT pointed at temp trees. This is steps 1 and 2 of the
multi-session design meeting: the login path allocates, the bridge reads.

Never touches the live logind, the real VTs, the real /run/systemd, or the
real cgroup hierarchy.

  ./tests/test_logind_session_alloc.py     exit 0 all pass, 1 any fail
"""
import os
import re
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
REGISTER = os.path.join(REPO, 'scripts', 'schema-session-register')
UNREGISTER = os.path.join(REPO, 'scripts', 'schema-session-unregister')
BUS_NAME = 'org.freedesktop.login1'
MANAGER_PATH = '/org/freedesktop/login1'
MANAGER_IFACE = 'org.freedesktop.login1.Manager'
SESSION_IFACE = 'org.freedesktop.login1.Session'
SETTLE = 1.2            # comfortably more than the 250 ms sync tick

results = []


def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def main():
    DBusGMainLoop(set_as_default=True)

    rundir = tempfile.mkdtemp(prefix='schema-alloc-run-')
    cgroot = tempfile.mkdtemp(prefix='schema-alloc-cg-')
    for d in ('sessions', 'seats', 'users'):
        os.makedirs(os.path.join(rundir, d), exist_ok=True)

    vtfile = tempfile.NamedTemporaryFile('w', suffix='.activevt', delete=False)
    vtfile.write('tty1\n')
    vtfile.close()

    senv = dict(os.environ)
    senv['SCHEMA_LOGIND_RUN_DIR'] = rundir
    senv['SCHEMA_CGROUP_ROOT'] = cgroot

    def register(*args):
        out = subprocess.run([REGISTER] + list(args), env=senv,
                             capture_output=True, text=True)
        return out.stdout.strip(), out.returncode

    def unregister(sid, uid=None):
        cmd = [UNREGISTER, str(sid)] + ([str(uid)] if uid else [])
        return subprocess.run(cmd, env=senv, capture_output=True,
                              text=True).returncode

    uid = str(os.getuid())
    user = subprocess.run(['id', '-un'], capture_output=True,
                          text=True).stdout.strip()

    daemon = subprocess.Popen(
        ['dbus-daemon', '--session', '--print-address', '--nofork'],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    addr = daemon.stdout.readline().strip()
    if not addr:
        print("could not start a private bus", file=sys.stderr)
        return 1

    env = dict(senv)
    env['DBUS_SYSTEM_BUS_ADDRESS'] = addr
    env['SCHEMA_LOGIND_ACTIVE_VT'] = vtfile.name
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

        def ids():
            return sorted(str(s[0]) for s in
                          mgr.ListSessions(dbus_interface=MANAGER_IFACE))

        def sprops(sid):
            obj = bus.get_object(BUS_NAME, '/org/freedesktop/login1/session/_%s' % sid)
            return obj.GetAll(SESSION_IFACE,
                              dbus_interface='org.freedesktop.DBus.Properties')

        print("-- an empty directory serves the legacy session --")
        check('starts on the synthesised id', ids() == ['31'], str(ids()))

        print("\n-- registering a GUI login allocates id 1 --")
        sid1, rc = register('--uid', uid, '--user', user, '--seat', 'seat0',
                            '--vtnr', '1', '--type', 'wayland', '--class',
                            'user', '--desktop', 'KDE', '--display',
                            '--service', 'sddm')
        check('register exits 0', rc == 0, 'rc=%d' % rc)
        check('allocates the lowest free id', sid1 == '1', sid1)
        scope1 = os.path.join(cgroot, 'user.slice', 'user-%s.slice' % uid,
                              'session-1.scope')
        check('creates the session scope cgroup', os.path.isdir(scope1), scope1)
        time.sleep(SETTLE)
        check('bridge replaces the synthesised session', ids() == ['1'], str(ids()))
        p = sprops('1')
        check('Id is the allocated id', str(p['Id']) == '1', str(p['Id']))
        check('Type came from the login path', str(p['Type']) == 'wayland',
              str(p['Type']))
        check('Leader is recorded', int(p['Leader']) > 0, str(p['Leader']))
        check('Active on the active VT', bool(p['Active']) is True,
              str(p['Active']))
        check('Scope matches the cgroup', str(p['Scope']) == 'session-1.scope',
              str(p['Scope']))

        print("\n-- a second, concurrent login gets its own id --")
        sid2, rc = register('--uid', uid, '--user', user, '--seat', 'seat0',
                            '--vtnr', '2', '--type', 'tty', '--service', 'login')
        check('allocates the next free id', sid2 == '2', sid2)
        time.sleep(SETTLE)
        check('both sessions on the bus', ids() == ['1', '2'], str(ids()))
        p2 = sprops('2')
        check('second session has its own VT', int(p2['VTNr']) == 2,
              str(p2['VTNr']))
        check('second session is not active', bool(p2['Active']) is False,
              str(p2['Active']))
        check('second session Type is tty', str(p2['Type']) == 'tty',
              str(p2['Type']))

        print("\n-- derived files describe both --")
        seat = open(os.path.join(rundir, 'seats', 'seat0')).read()
        check('seat file lists both sessions', 'SESSIONS=1 2' in seat,
              seat.replace('\n', ' '))
        check('seat file names the active one', 'ACTIVE=1\n' in seat,
              seat.replace('\n', ' '))

        # --- LEADER_STARTTIME baseline is recorded for the recycle guard ---
        sid_lt, rc_lt = register(
            '--uid', uid, '--user', user, '--seat', 'seat0', '--vtnr', '7',
            '--type', 'tty', '--leader', str(os.getpid()))
        check('register with a live leader exits 0', rc_lt == 0, 'rc=%d' % rc_lt)
        body = open(os.path.join(rundir, 'sessions', sid_lt)).read()
        with open('/proc/%d/stat' % os.getpid()) as f:
            want = int(f.read().rsplit(') ', 1)[1].split()[19])
        check('session file records LEADER_STARTTIME',
              ('LEADER_STARTTIME=%d' % want) in body.splitlines(),
              body)
        time.sleep(SETTLE)
        unregister(sid_lt, uid)
        time.sleep(SETTLE)

        print("\n-- unregistering releases the id and the scope --")
        rc = unregister('1', uid)
        check('unregister exits 0', rc == 0, 'rc=%d' % rc)
        check('state file is gone',
              not os.path.exists(os.path.join(rundir, 'sessions', '1')))
        check('scope cgroup is gone', not os.path.isdir(scope1), scope1)
        time.sleep(SETTLE)
        check('bridge drops the session', ids() == ['2'], str(ids()))

        print("\n-- a released id is reused --")
        sid3, _ = register('--uid', uid, '--user', user, '--vtnr', '3',
                           '--type', 'tty')
        check('reuses the lowest free id', sid3 == '1', sid3)
        time.sleep(SETTLE)
        check('bridge picks it up again', ids() == ['1', '2'], str(ids()))

        print("\n-- releasing every session falls back to legacy --")
        unregister('1', uid)
        unregister('2', uid)
        time.sleep(SETTLE)
        check('empty directory synthesises 31 again', ids() == ['31'],
              str(ids()))

        print("\n-- the bridge never showed a half-built session --")
        log = ''
        stub.terminate()
        try:
            log = stub.stdout.read()
        except Exception:
            pass
        seen = re.findall(r'Registered Session (\S+) at', log)
        bad = [s for s in seen if s not in ('1', '2', '31', sid_lt)]
        check('no session object for an unexpected id', not bad,
              'saw %s' % (seen or 'nothing'))

    finally:
        for p in (stub, daemon):
            try:
                p.terminate()
                p.wait(timeout=5)
            except Exception:
                p.kill()
        os.unlink(vtfile.name)
        shutil.rmtree(rundir, ignore_errors=True)
        shutil.rmtree(cgroot, ignore_errors=True)

    failed = [r for r in results if not r[1]]
    print(f"\n>> {len(results) - len(failed)}/{len(results)} passed")
    print(">> RESULT: " + ("PASS" if not failed
                           else "FAIL — " + ", ".join(r[0] for r in failed)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
