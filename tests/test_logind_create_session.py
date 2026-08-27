#!/usr/bin/env python3
"""CreateSession tests for schema-logind.

pam_systemd.so already calls Manager.CreateSession on every login -- it sits in
the session stack at /etc/pam.d/system-auth, which /etc/pam.d/login includes --
and before this it died with UnknownMethod behind '-session optional'. These
tests drive that call the way pam_systemd does, with the argument list captured
off the live bus.

Also covers what it must REFUSE: sudo and su call this too, with
class=background/background-light and no tty, and inventing seat sessions for
those would fill `loginctl list-sessions` with noise for every sudo.

Never touches the live logind, the real /run/systemd, or the real cgroups.

  ./tests/test_logind_create_session.py     exit 0 all pass, 1 any fail
"""
import os
import pwd
import shutil
import subprocess
import sys
import tempfile
import time

import dbus
from dbus.mainloop.glib import DBusGMainLoop

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGIND = os.path.join(REPO, 'scripts', 'schema-logind.py')
BUS_NAME = 'org.freedesktop.login1'
MANAGER_PATH = '/org/freedesktop/login1'
MANAGER_IFACE = 'org.freedesktop.login1.Manager'
SESSION_IFACE = 'org.freedesktop.login1.Session'
SETTLE = 1.2

results = []


def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def main():
    DBusGMainLoop(set_as_default=True)

    rundir = tempfile.mkdtemp(prefix='schema-cs-run-')
    cgroot = tempfile.mkdtemp(prefix='schema-cs-cg-')
    for d in ('sessions', 'seats', 'users'):
        os.makedirs(os.path.join(rundir, d), exist_ok=True)

    vtfile = tempfile.NamedTemporaryFile('w', suffix='.activevt', delete=False)
    vtfile.write('tty2\n')
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
    env['SCHEMA_LOGIND_RUN_DIR'] = rundir
    env['SCHEMA_CGROUP_ROOT'] = cgroot
    env.pop('SCHEMA_LOGIND_VTNR', None)

    stub = subprocess.Popen([sys.executable, LOGIND], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    bus = dbus.bus.BusConnection(addr)

    # A live child to be the session leader, so the reaper does not eat the
    # session mid-test. Reaping is pid-based and runs every tick now.
    leader = subprocess.Popen(['sleep', '120'])
    uid = os.getuid()

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

        def create(pid=0, service='login', type_='unspecified', class_='user',
                   desktop='', seat='seat0', vtnr=0, tty='', display='',
                   remote=False, ruser='', rhost=''):
            return mgr.CreateSession(
                dbus.UInt32(uid), dbus.UInt32(pid), service, type_, class_,
                desktop, seat, dbus.UInt32(vtnr), tty, display,
                dbus.Boolean(remote), ruser, rhost,
                dbus.Array([], signature='(sv)'),
                dbus_interface=MANAGER_IFACE)

        def ids():
            return sorted(str(s[0]) for s in
                          mgr.ListSessions(dbus_interface=MANAGER_IFACE))

        print("-- a tty login gets a session --")
        r = create(pid=leader.pid, service='login', class_='user', tty='tty2',
                   vtnr=2)
        sid = str(r[0])
        check('returns an id', sid.isdigit(), sid)
        check('returns the object path',
              str(r[1]) == '/org/freedesktop/login1/session/_%s' % sid, str(r[1]))
        check('returns the runtime path',
              str(r[2]) == '/run/user/%d' % uid, str(r[2]))
        check('returns a usable fifo fd', r[3].take() >= 0, 'fd')
        check('returns the uid', int(r[4]) == uid, str(r[4]))
        check('returns the seat', str(r[5]) == 'seat0', str(r[5]))
        check('returns the vtnr', int(r[6]) == 2, str(r[6]))
        check('is not flagged existing', bool(r[7]) is False, str(r[7]))

        time.sleep(SETTLE)
        check('session is on the bus', sid in ids(), str(ids()))
        obj = bus.get_object(BUS_NAME, '/org/freedesktop/login1/session/_%s' % sid)
        p = obj.GetAll(SESSION_IFACE,
                       dbus_interface='org.freedesktop.DBus.Properties')
        check('Leader is the pid we passed', int(p['Leader']) == leader.pid,
              str(p['Leader']))
        check('Type defaults to tty', str(p['Type']) == 'tty', str(p['Type']))
        check('Class is user', str(p['Class']) == 'user', str(p['Class']))
        check('Active on the active VT', bool(p['Active']) is True,
              str(p['Active']))
        scope = os.path.join(cgroot, 'user.slice', 'user-%d.slice' % uid,
                             'session-%s.scope' % sid)
        check('scope cgroup created', os.path.isdir(scope), scope)
        procs = os.path.join(scope, 'cgroup.procs')
        check('leader put in the scope',
              os.path.exists(procs) and str(leader.pid) in open(procs).read(),
              open(procs).read().strip() if os.path.exists(procs) else 'missing')

        print("\n-- CreateSession writes the same file schema-session-register would --")
        # Drive the helper directly into a throwaway run dir with the SAME args
        # CreateSession maps, then compare the resulting state file key-for-key
        # (except the two timestamps, which are wall-clock and always differ).
        ref_run = tempfile.mkdtemp(prefix='ref-run-')
        ref_cg = tempfile.mkdtemp(prefix='ref-cg-')
        helper = os.path.join(REPO, 'scripts', 'schema-session-register')
        henv = dict(os.environ)
        henv['SCHEMA_LOGIND_RUN_DIR'] = ref_run
        henv['SCHEMA_CGROUP_ROOT'] = ref_cg
        henv['SCHEMA_LOGIND_ACTIVE_VT'] = os.environ.get('SCHEMA_LOGIND_ACTIVE_VT', '')
        # CreateSession sends the resolved username (matching real logind
        # session files), not the raw uid -- match that here so the two
        # calls are actually comparable.
        ref_sid = subprocess.check_output(
            [helper, '--uid', str(uid), '--user', pwd.getpwuid(uid).pw_name,
             '--seat', 'seat0', '--vtnr', '2', '--type', 'tty', '--class', 'user',
             '--service', 'login', '--leader', str(leader.pid)],
            env=henv, text=True).strip()

        def stable_keys(path):
            d = {}
            for line in open(path):
                if '=' in line and not line.startswith('#'):
                    k, v = line.rstrip('\n').split('=', 1)
                    # REALTIME/MONOTONIC are wall-clock, always differ.
                    # ACTIVE/STATE are live state the daemon's VT-poll loop
                    # (on_vt_changed -> _write_back_active) keeps in sync with
                    # the real active VT independently of whoever wrote the
                    # file, so the CreateSession-managed file can legitimately
                    # diverge from a one-off standalone helper invocation that
                    # nothing is watching.
                    if k not in ('REALTIME', 'MONOTONIC', 'ACTIVE', 'STATE'):
                        d[k] = v
            return d

        cs_file = os.path.join(rundir, 'sessions', sid)      # written by CreateSession above
        ref_file = os.path.join(ref_run, 'sessions', ref_sid)
        check('CreateSession state file matches the helper key-for-key',
              stable_keys(cs_file) == stable_keys(ref_file),
              '%s vs %s' % (stable_keys(cs_file), stable_keys(ref_file)))
        shutil.rmtree(ref_run, ignore_errors=True)
        shutil.rmtree(ref_cg, ignore_errors=True)

        print("\n-- sudo and su are refused, not given fake sessions --")
        before = ids()
        for svc, cls in (('sudo', 'background-light'), ('su-l', 'background'),
                         ('sshd', 'user')):
            try:
                create(pid=leader.pid, service=svc, class_=cls, tty='')
                check('%s (class=%s) is refused' % (svc, cls), False,
                      'a session was created')
            except dbus.DBusException as e:
                check('%s (class=%s) is refused' % (svc, cls),
                      'NotSupported' in e.get_dbus_name(), e.get_dbus_name())
        check('no sessions were created for them', ids() == before, str(ids()))

        print("\n-- a pts login is refused too --")
        try:
            create(pid=leader.pid, service='sshd', class_='user', tty='pts/3')
            check('pts tty is refused', False, 'a session was created')
        except dbus.DBusException as e:
            check('pts tty is refused', 'NotSupported' in e.get_dbus_name(),
                  e.get_dbus_name())

        print("\n-- vtnr is recovered from the tty name --")
        leader2 = subprocess.Popen(['sleep', '120'])
        try:
            r2 = create(pid=leader2.pid, service='login', class_='user',
                        tty='tty4', vtnr=0)
            sid2 = str(r2[0])
            check('tty4 with vtnr=0 still resolves', int(r2[6]) == 4, str(r2[6]))
            check('distinct id from the first', sid2 != sid, '%s vs %s' % (sid2, sid))
            time.sleep(SETTLE)
            check('both sessions on the bus',
                  {sid, sid2} <= set(ids()), str(ids()))

            print("\n-- the same user on the same VT is reused, not duplicated --")
            r3 = create(pid=leader2.pid, service='login', class_='user',
                        tty='tty4', vtnr=4)
            check('flagged existing', bool(r3[7]) is True, str(r3[7]))
            check('same id returned', str(r3[0]) == sid2,
                  '%s vs %s' % (r3[0], sid2))

            print("\n-- a dead leader is reaped --")
            # Real cgroupfs drops a scope's auto-created files when the
            # directory is removed, so rmdir on an emptied scope succeeds.
            # A plain temp tree keeps cgroup.procs as an ordinary file and
            # rmdir would fail with ENOTEMPTY for a reason the kernel would
            # never produce, so remove it here rather than weaken the check.
            scope2 = os.path.join(cgroot, 'user.slice', 'user-%d.slice' % uid,
                                  'session-%s.scope' % sid2)
            try:
                os.unlink(os.path.join(scope2, 'cgroup.procs'))
            except OSError:
                pass
            leader2.terminate()
            leader2.wait(timeout=5)
            time.sleep(SETTLE)
            check('session dropped from the bus', sid2 not in ids(), str(ids()))
            check('state file removed',
                  not os.path.exists(os.path.join(rundir, 'sessions', sid2)))
            check('scope cgroup removed',
                  not os.path.isdir(os.path.join(
                      cgroot, 'user.slice', 'user-%d.slice' % uid,
                      'session-%s.scope' % sid2)))
        finally:
            if leader2.poll() is None:
                leader2.kill()

        print("\n-- ReleaseSession removes the session --")
        mgr.ReleaseSession(sid, dbus_interface=MANAGER_IFACE)
        time.sleep(SETTLE)
        check('released session is gone', sid not in ids(), str(ids()))

        print("\n-- ReleaseSession removes the state file and the scope --")
        # rel_leader is kept ALIVE across the ReleaseSession call (never
        # terminated first): a live leader means the dead-leader reaper in
        # SessionRegistry.sync() (runs every ~250ms, same one the harness's
        # top-level `leader` sidesteps with its own `sleep 120`) will not
        # touch this session on its own. That isolates the assertions below
        # to ReleaseSession's own teardown -- if ReleaseSession silently did
        # nothing, the reaper could not mask that by cleaning up behind it.
        rel_leader = subprocess.Popen(['sleep', '120'])
        try:
            rr = create(pid=rel_leader.pid, service='login', class_='user',
                        tty='tty7', vtnr=7)
            rsid = str(rr[0])
            time.sleep(SETTLE)
            rfile = os.path.join(rundir, 'sessions', rsid)
            rscope = os.path.join(cgroot, 'user.slice', 'user-%d.slice' % uid,
                                  'session-%s.scope' % rsid)
            check('release: file present before', os.path.exists(rfile), rfile)
            # Real cgroupfs drops a scope's auto-created cgroup.procs when the
            # leader dies, so rmdir on an emptied scope succeeds. A plain temp
            # tree keeps cgroup.procs as an ordinary file even with a live
            # leader (same caveat as "a dead leader is reaped" above), so
            # clear it here rather than weaken the check -- this must not
            # depend on the leader dying, since the leader stays alive.
            try:
                os.unlink(os.path.join(rscope, 'cgroup.procs'))
            except OSError:
                pass
            mgr.ReleaseSession(rsid, dbus_interface=MANAGER_IFACE)
            time.sleep(SETTLE)
            check('release: state file removed', not os.path.exists(rfile), rfile)
            check('release: scope rmdir-ed', not os.path.isdir(rscope), rscope)
        finally:
            if rel_leader.poll() is None:
                rel_leader.terminate()
                rel_leader.wait(timeout=5)

    finally:
        for p in (leader, stub, daemon):
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
