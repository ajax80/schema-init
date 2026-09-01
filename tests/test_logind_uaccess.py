#!/usr/bin/env python3
"""Login-time uaccess re-scan tests for schema-logind.

Coldplug applies uaccess ACLs before anyone is logged in, so a device present
at boot only ever gets its ACL granted to whoever was active then (nobody).
logind — like real systemd's logind-acl.c devnode_acl_all — must re-apply on
seat-active-uid change. This covers node enumeration, the setfacl apply, and
the registry change-detection that drives it.

Temp trees via SCHEMA_LOGIND_{RUN_DIR,UDEV_DATA,DEV_DIR}; real setfacl/getfacl
on stand-in files (ACLs apply to any file, no root needed).

  ./tests/test_logind_uaccess.py     exit 0 all pass, 1 any fail
"""
import os
import sys
import tempfile
import subprocess
import importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RUNDIR = tempfile.mkdtemp(prefix='schema-logind-uaccess-')
for _d in ('sessions', 'seats', 'users'):
    os.makedirs(os.path.join(RUNDIR, _d), exist_ok=True)
UDEV = os.path.join(RUNDIR, 'udev-data'); os.makedirs(UDEV)
DEV = os.path.join(RUNDIR, 'dev'); os.makedirs(DEV)
os.environ['SCHEMA_LOGIND_RUN_DIR'] = RUNDIR
os.environ['SCHEMA_LOGIND_UDEV_DATA'] = UDEV
os.environ['SCHEMA_LOGIND_DEV_DIR'] = DEV

spec = importlib.util.spec_from_file_location(
    'schema_logind', os.path.join(REPO, 'scripts', 'schema-logind.py'))
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

uid = os.getuid()
results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def node(name):
    p = os.path.join(DEV, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'w').close()
    return p


def record(fname, body):
    with open(os.path.join(UDEV, fname), 'w') as f:
        f.write(body)


def getfacl(p):
    return subprocess.run(['getfacl', '-pn', p], capture_output=True, text=True).stdout


# -- node enumeration --------------------------------------------------------

def test_enumerate():
    sdr = node('bus/usb/002/005')      # RTL-SDR, seat0 by default (no ID_SEAT)
    hid = node('hidraw9')              # FIDO2 key, explicit ID_SEAT=seat0
    other = node('sda')                # not uaccess
    otherseat = node('ttyS9')          # uaccess but a different seat
    record('c189:132', 'N:bus/usb/002/005\nG:uaccess\nQ:uaccess\nQ:seat\n')
    record('c239:0', 'N:hidraw9\nE:ID_SEAT=seat0\nQ:uaccess\nQ:seat\n')
    record('b8:0', 'N:sda\nQ:seat\n')
    record('c4:73', 'N:ttyS9\nE:ID_SEAT=seat1\nQ:uaccess\nQ:seat\n')

    got = set(L.uaccess_seat_nodes('seat0'))
    check('enumerate: seat0 uaccess nodes found', got == {sdr, hid},
          str(sorted(got)))
    check('enumerate: non-uaccess excluded', other not in got)
    check('enumerate: other-seat excluded', otherseat not in got)
    check('enumerate: seat1 sees only its node',
          set(L.uaccess_seat_nodes('seat1')) == {otherseat},
          str(L.uaccess_seat_nodes('seat1')))


# -- setfacl apply -----------------------------------------------------------

def test_apply():
    a = node('apply/a'); b = node('apply/b')
    for p in (a, b):
        subprocess.run(['setfacl', '-b', p], check=True)
    L.apply_uaccess_acl([a, b], uid, None)
    check('apply: new uid granted rw', all(f'user:{uid}:rw' in getfacl(p) for p in (a, b)))

    # a later login by uid+1 revokes the old, grants the new
    L.apply_uaccess_acl([a, b], uid + 1, uid)
    check('apply: new login granted', all(f'user:{uid + 1}:rw' in getfacl(p) for p in (a, b)))
    check('apply: prior login revoked', all(f'user:{uid}:rw' not in getfacl(p) for p in (a, b)))

    # idempotent re-apply of the same uid is a no-op that still holds
    L.apply_uaccess_acl([a, b], uid + 1, uid + 1)
    check('apply: idempotent', all(f'user:{uid + 1}:rw' in getfacl(p) for p in (a, b)))


# -- registry change-detection ----------------------------------------------

def _set_active(reg, active_uid, sid='1'):
    body = ['# This is private data. Do not parse.']
    for k, v in dict(UID=active_uid, USER='u%d' % active_uid, SEAT='seat0',
                     VTNR=1, ACTIVE=1, STATE='active', IS_DISPLAY=1).items():
        body.append('%s=%s' % (k, v))
    with open(os.path.join(RUNDIR, 'sessions', sid), 'w') as f:
        f.write('\n'.join(body) + '\n')
    reg.sessions = {}
    for s, rec in L.scan_session_files().items():
        obj = type('S', (), {})()
        obj.sid, obj.record, obj.path = s, rec, L.session_path_for(s)
        obj.active = rec.active
        reg.sessions[s] = obj


def test_registry_fires_on_change():
    calls = []
    real_nodes, real_apply = L.uaccess_seat_nodes, L.apply_uaccess_acl
    L.uaccess_seat_nodes = lambda seat='seat0': ['/dev/fake0']
    L.apply_uaccess_acl = lambda nodes, new, old: calls.append((tuple(nodes), new, old))
    try:
        reg = L.SessionRegistry.__new__(L.SessionRegistry)
        reg.sessions = {}; reg._derived = {}; reg._seat_active_uid = {}

        _set_active(reg, 1000)
        reg._write_derived()
        check('registry: fires on first active uid',
              calls == [(('/dev/fake0',), 1000, None)], str(calls))

        calls.clear()
        reg._write_derived()
        check('registry: no re-fire when uid unchanged', calls == [], str(calls))

        calls.clear()
        _set_active(reg, 1001)
        reg._write_derived()
        check('registry: re-fires with old uid on change',
              calls == [(('/dev/fake0',), 1001, 1000)], str(calls))
    finally:
        L.uaccess_seat_nodes, L.apply_uaccess_acl = real_nodes, real_apply


def main():
    print(f"schema-logind uaccess re-scan tests (run dir {RUNDIR})\n")
    for fn in (test_enumerate, test_apply, test_registry_fires_on_change):
        print(fn.__name__)
        fn()
        print()
    passed = sum(1 for r in results if r)
    print(f"{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
