#!/usr/bin/env python3
"""schema-logind: a session whose file exists is visible to a lookup at once.

The registry learns of /run/systemd/sessions/<id> on a 250 ms poll. A
compositor started by the login script asks GetSessionByPID / GetSession a few
ms after the file lands; answered from the last poll it got NoSuchSession, or
the seat's active session -- the synthesised placeholder #31 -- and took
control of that: the boot race the doctor's session-single check names.

Also: a cold start with no sessions yet is not an "ORPHANED SESSION".

Own private bus, own rundir; never touches the live logind.

  ./tests/test_logind_lookup_resync.py     exit 0 all pass, 1 any fail
"""
import os
import pwd
import shutil
import subprocess
import sys
import tempfile
import time

import dbus

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGIND = os.path.join(REPO, 'scripts', 'schema-logind.py')
MANAGER_IFACE = 'org.freedesktop.login1.Manager'

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def write_session(rundir, sid, leader):
    uid = os.getuid()
    tmp = os.path.join(rundir, 'sessions', '.%s.tmp' % sid)
    with open(tmp, 'w') as f:
        f.write('# This is private data. Do not parse.\n'
                'UID=%d\nUSER=%s\nACTIVE=1\nSTATE=active\nSEAT=seat0\nVTNR=2\n'
                'TYPE=wayland\nCLASS=user\nDESKTOP=KDE\nIS_DISPLAY=1\nREMOTE=0\n'
                'LEADER=%d\nSERVICE=schema-autologin\n'
                % (uid, pwd.getpwuid(uid).pw_name, leader))
    # the login script's atomic rename: the file appears whole
    os.rename(tmp, os.path.join(rundir, 'sessions', sid))


def main():
    rundir = tempfile.mkdtemp(prefix='schema-lr-run-')
    cgroot = tempfile.mkdtemp(prefix='schema-lr-cg-')
    os.makedirs(os.path.join(rundir, 'sessions'))
    vtfile = os.path.join(rundir, 'activevt')
    with open(vtfile, 'w') as f:
        f.write('tty2\n')
    busd = stub = None
    leaders = []
    try:
        busd = subprocess.Popen(['dbus-daemon', '--session', '--print-address', '--nofork'],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        addr = busd.stdout.readline().strip()
        env = dict(os.environ)
        env.update({'DBUS_SYSTEM_BUS_ADDRESS': addr, 'SCHEMA_LOGIND_ACTIVE_VT': vtfile,
                    'SCHEMA_LOGIND_RUN_DIR': rundir, 'SCHEMA_LOGIND_UDEV_DATA': rundir,
                    'SCHEMA_LOGIND_DEV_DIR': rundir, 'SCHEMA_CGROUP_ROOT': cgroot})
        env.pop('SCHEMA_LOGIND_VTNR', None)
        # a legacy #31 under a logind restarted long after boot still warns
        late_log = open(os.path.join(rundir, 'late'), 'w+')
        late = subprocess.Popen([sys.executable, LOGIND], env=dict(env, SCHEMA_LOGIND_UPTIME='500'),
                                stdout=late_log, stderr=subprocess.STDOUT, text=True)
        for _ in range(100):
            late_log.seek(0)
            if 'watching /sys/class/tty' in late_log.read():
                break
            time.sleep(0.05)
        late.terminate()
        late.wait(5)
        late_log.seek(0)
        check('placeholder long after boot: ORPHANED SESSION still warned',
              'ORPHANED SESSION' in late_log.read())
        env['SCHEMA_LOGIND_UPTIME'] = '5'
        log = open(os.path.join(rundir, 'out'), 'w+')
        stub = subprocess.Popen([sys.executable, LOGIND], env=env, stdout=log,
                                stderr=subprocess.STDOUT, text=True)
        bus = dbus.bus.BusConnection(addr)
        mgr = None
        for _ in range(100):
            try:
                mgr = dbus.Interface(bus.get_object('org.freedesktop.login1',
                                                    '/org/freedesktop/login1'), MANAGER_IFACE)
                mgr.ListSessions()
                break
            except dbus.exceptions.DBusException:
                mgr = None
                time.sleep(0.05)
        if not check('daemon up on the private bus', mgr is not None):
            return

        log.seek(0)
        check('cold boot with no sessions: no ORPHANED SESSION warning',
              'ORPHANED SESSION' not in log.read())

        # the autologin script's poke: GetSession once, then the seat already
        # names the real session -- never the placeholder -- with no wait
        seat = dbus.Interface(bus.get_object('org.freedesktop.login1',
                                             '/org/freedesktop/login1/seat/seat0'),
                              'org.freedesktop.DBus.Properties')
        first = subprocess.Popen(['sleep', '60'], start_new_session=True)
        leaders.append(first)
        write_session(rundir, '30', first.pid)
        try:
            mgr.GetSession('30')
            act = str(seat.Get('org.freedesktop.login1.Seat', 'ActiveSession')[0])
        except dbus.exceptions.DBusException as e:
            act = e.get_dbus_name()
        check('after the poke the seat names the real session, not #31', act == '30', act)

        # a session leader of its own (getsid() == its pid), as the login script's is
        misses = []
        for n in range(5):
            leader = subprocess.Popen(['sleep', '60'], start_new_session=True)
            leaders.append(leader)
            sid = str(40 + n)
            write_session(rundir, sid, leader.pid)
            try:
                p = mgr.GetSessionByPID(dbus.UInt32(leader.pid))
                if not str(p).endswith('/_' + sid):
                    misses.append('%s by pid -> %s' % (sid, p))
            except dbus.exceptions.DBusException as e:
                misses.append('%s by pid: %s' % (sid, e.get_dbus_name()))
            sid2 = str(50 + n)
            write_session(rundir, sid2, leader.pid)
            try:
                mgr.GetSession(sid2)
            except dbus.exceptions.DBusException as e:
                misses.append('%s by id: %s' % (sid2, e.get_dbus_name()))
        check('a just-written session answers GetSessionByPID / GetSession at once',
              not misses, '; '.join(misses))

        try:
            mgr.GetSession('999')
            check('an id with no file is still NoSuchSession', False, 'answered')
        except dbus.exceptions.DBusException as e:
            check('an id with no file is still NoSuchSession',
                  e.get_dbus_name() == 'org.freedesktop.login1.NoSuchSession', e.get_dbus_name())
    finally:
        for p in leaders:
            p.kill()
        if stub:
            stub.terminate()
            try:
                stub.wait(5)
            except subprocess.TimeoutExpired:
                stub.kill()
        if busd:
            busd.kill()
        shutil.rmtree(rundir, ignore_errors=True)
        shutil.rmtree(cgroot, ignore_errors=True)


main()
print('test_logind_lookup_resync: %s' % ('OK' if all(results) else 'FAIL'))
sys.exit(0 if results and all(results) else 1)
