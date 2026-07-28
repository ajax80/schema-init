#!/usr/bin/env python3
"""Session-registry tests for schema-logind.

Exercises the /run/systemd/sessions projection without a bus and without
touching live state: SCHEMA_LOGIND_RUN_DIR points the module at a temp tree
before it is imported.

Covers what the multi-session bridge has to get right before it can go near a
real login: parsing, the legacy fallback for an empty directory, laziness of
the /proc-walking fallbacks, change detection, and the derived seat/user files.

  ./tests/test_logind_registry.py        exit 0 all pass, 1 any fail
"""
import os
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RUNDIR = tempfile.mkdtemp(prefix='schema-logind-test-')
for _d in ('sessions', 'seats', 'users'):
    os.makedirs(os.path.join(RUNDIR, _d), exist_ok=True)
os.environ['SCHEMA_LOGIND_RUN_DIR'] = RUNDIR

sys.path.insert(0, os.path.join(REPO, 'scripts'))
import importlib.util
spec = importlib.util.spec_from_file_location(
    'schema_logind', os.path.join(REPO, 'scripts', 'schema-logind.py'))
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

results = []


def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def write_session(sid, **kv):
    body = ['# This is private data. Do not parse.']
    body += ['%s=%s' % (k, v) for k, v in kv.items()]
    with open(os.path.join(RUNDIR, 'sessions', str(sid)), 'w') as f:
        f.write('\n'.join(body) + '\n')


def clear_sessions():
    d = os.path.join(RUNDIR, 'sessions')
    for name in os.listdir(d):
        os.unlink(os.path.join(d, name))


# -- parsing ----------------------------------------------------------------

def test_parse():
    clear_sessions()
    write_session(7, UID=1000, USER='ajax80', SEAT='seat0', VTNR=2,
                  TYPE='wayland', CLASS='user', DESKTOP='KDE', STATE='active',
                  ACTIVE=1, REMOTE=0, IS_DISPLAY=1)
    recs = L.scan_session_files()
    check('parse: one session found', list(recs) == ['7'], str(list(recs)))
    r = recs['7']
    check('parse: uid', r.uid == 1000, str(r.uid))
    check('parse: user', r.user == 'ajax80', r.user)
    check('parse: vtnr', r.vtnr == 2, str(r.vtnr))
    check('parse: type', r.type == 'wayland', r.type)
    check('parse: desktop', r.desktop == 'KDE', r.desktop)
    check('parse: class', r.klass == 'user', r.klass)
    check('parse: active', r.active is True, str(r.active))
    check('parse: is_display', r.is_display is True, str(r.is_display))
    check('parse: not synthesised', r.synthesised is False, str(r.synthesised))


def test_legacy_fallback():
    """An empty directory must still yield the legacy session, so the bridge
    works against a login script that never learned to allocate an id."""
    clear_sessions()
    recs = L.scan_session_files()
    check('legacy: synthesises one session',
          list(recs) == [L.LEGACY_SESSION_ID], str(list(recs)))
    check('legacy: marked synthesised',
          recs[L.LEGACY_SESSION_ID].synthesised is True)
    check('legacy: path is the historical one',
          L.session_path_for(L.LEGACY_SESSION_ID)
          == '/org/freedesktop/login1/session/_31',
          L.session_path_for(L.LEGACY_SESSION_ID))


def test_multiple():
    clear_sessions()
    write_session(1, UID=1000, USER='ajax80', VTNR=1, ACTIVE=1, STATE='active',
                  IS_DISPLAY=1)
    write_session(2, UID=1001, USER='other', VTNR=2, ACTIVE=0, STATE='online',
                  TYPE='tty')
    recs = L.scan_session_files()
    check('multi: both found', sorted(recs) == ['1', '2'], str(sorted(recs)))
    check('multi: distinct uids',
          (recs['1'].uid, recs['2'].uid) == (1000, 1001))
    check('multi: distinct vtnr',
          (recs['1'].vtnr, recs['2'].vtnr) == (1, 2))
    check('multi: only one active',
          [s for s, r in recs.items() if r.active] == ['1'])


def test_lazy_fallbacks():
    """The expensive fallbacks walk all of /proc and the registry rebuilds
    records at 4 Hz, so a scan must not touch them. Both prior CPU-spin
    incidents in this codebase came from work on a timer."""
    clear_sessions()
    write_session(3, UID=1000, USER='ajax80', VTNR=1, TYPE='wayland',
                  DESKTOP='KDE', STATE='active', ACTIVE=1)

    calls = []
    real_leader, real_desktop = L.get_session_leader, L.get_desktop_name
    L.get_session_leader = lambda: (calls.append('leader'), (0, ''))[1]
    L.get_desktop_name = lambda: (calls.append('desktop'), '')[1]
    try:
        recs = L.scan_session_files()
        for _ in range(20):                  # five seconds of polling
            recs = L.scan_session_files()
            for r in recs.values():
                r.signature()
        check('lazy: scan+signature walks /proc zero times',
              calls == [], str(calls))

        # ...but the value is still there when a property actually asks.
        _ = recs['3'].leader
        check('lazy: resolved on demand', calls == ['leader'], str(calls))
        _ = recs['3'].leader
        check('lazy: cached after first resolve', calls == ['leader'], str(calls))
    finally:
        L.get_session_leader, L.get_desktop_name = real_leader, real_desktop


def test_signature_and_changes():
    clear_sessions()
    write_session(4, UID=1000, USER='ajax80', VTNR=1, ACTIVE=1, STATE='active')
    a = L.scan_session_files()['4']
    b = L.scan_session_files()['4']
    check('change: identical file -> identical signature',
          a.signature() == b.signature())
    check('change: identical file -> no changed properties',
          a.changed_properties(b) == [], str(a.changed_properties(b)))

    write_session(4, UID=1000, USER='ajax80', VTNR=1, ACTIVE=0, STATE='online')
    c = L.scan_session_files()['4']
    check('change: differing file -> differing signature',
          a.signature() != c.signature())
    check('change: reports Active and State',
          sorted(c.changed_properties(a)) == ['Active', 'State'],
          str(sorted(c.changed_properties(a))))


def test_leader_sweep():
    clear_sessions()
    write_session(5, UID=1000, USER='ajax80', VTNR=1, LEADER=os.getpid())
    write_session(6, UID=1000, USER='ajax80', VTNR=3, LEADER=999999)
    recs = L.scan_session_files()
    check('sweep: live leader survives', recs['5'].leader_alive() is True)
    check('sweep: dead leader detected', recs['6'].leader_alive() is False)

    clear_sessions()
    write_session(8, UID=1000, USER='ajax80', VTNR=1)
    recs = L.scan_session_files()
    check('sweep: no LEADER key is not treated as dead',
          recs['8'].leader_alive() is True)


def test_derived_files():
    """The seat and user files are what sd_session_*/sd_seat_* read; they were
    created empty and never populated before this."""
    clear_sessions()
    write_session(1, UID=1000, USER='ajax80', SEAT='seat0', VTNR=1, ACTIVE=1,
                  STATE='active', IS_DISPLAY=1)
    write_session(2, UID=1001, USER='other', SEAT='seat0', VTNR=2, ACTIVE=0,
                  STATE='online')

    reg = L.SessionRegistry.__new__(L.SessionRegistry)
    reg.sessions = {}
    reg._derived = {}
    for sid, rec in L.scan_session_files().items():
        obj = type('S', (), {})()
        obj.sid, obj.record, obj.path = sid, rec, L.session_path_for(sid)
        obj.active = rec.active
        reg.sessions[sid] = obj
    reg._write_derived()

    seat = L.read_state_file(os.path.join(RUNDIR, 'seats', 'seat0'))
    check('derived: seat ACTIVE is the active session',
          seat.get('ACTIVE') == '1', str(seat))
    check('derived: seat lists both sessions',
          sorted(seat.get('SESSIONS', '').split()) == ['1', '2'], str(seat))
    check('derived: seat lists both uids',
          sorted(seat.get('UIDS', '').split()) == ['1000', '1001'], str(seat))

    u1000 = L.read_state_file(os.path.join(RUNDIR, 'users', '1000'))
    check('derived: active user is active',
          u1000.get('STATE') == 'active', str(u1000))
    check('derived: user names its session',
          u1000.get('SESSIONS') == '1', str(u1000))
    u1001 = L.read_state_file(os.path.join(RUNDIR, 'users', '1001'))
    check('derived: inactive user is online',
          u1001.get('STATE') == 'online', str(u1001))

    before = dict(reg._derived)
    reg._write_derived()
    check('derived: unchanged state rewrites nothing',
          reg._derived == before)


def main():
    print(f"schema-logind registry tests (run dir {RUNDIR})\n")
    for fn in (test_parse, test_legacy_fallback, test_multiple,
               test_lazy_fallbacks, test_signature_and_changes,
               test_leader_sweep, test_derived_files):
        print(fn.__name__)
        fn()
        print()
    failed = [r for r in results if not r[1]]
    print(f"{len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
