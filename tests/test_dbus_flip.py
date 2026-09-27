#!/usr/bin/env python3
"""schema-dbus-flip.sh arm/rollback against a temp root.

check runs the real broker on a scratch socket against this box's real
busconfig, so this needs schema-dbus and dbus-send installed; it never
touches the live bus. Everything the script writes lands under a temp root.

  ./tests/test_dbus_flip.py     exit 0 all pass, 1 any fail
"""
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STOCK_SVC = os.path.join(REPO, 'distros', 'fedora-installer', 'rail', 'services', 'dbus.svc')

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))


def main():
    if not any(os.access(p, os.X_OK) for p in ('/usr/local/bin/schema-dbus', '/usr/bin/schema-dbus')):
        print("SKIP: schema-dbus not installed")
        return 0

    tmp = tempfile.mkdtemp(prefix='schema-dbus-flip-')
    lib = os.path.join(tmp, 'lib')
    root = os.path.join(tmp, 'root')
    os.makedirs(lib)
    for src in ('scripts/schema-dbus-flip.sh', 'scripts/schema-dbus-run.sh',
                'tools/dbus-learn/dissect_policy.py'):
        shutil.copy2(os.path.join(REPO, src), lib)
    flip = os.path.join(lib, 'schema-dbus-flip.sh')
    run_sh = os.path.join(lib, 'schema-dbus-run.sh')
    svc = os.path.join(root, 'etc/schema-init/services/dbus.svc')
    stock = os.path.join(root, 'var/lib/schema-init/dbus.svc.stock')
    gate = os.path.join(root, 'etc/schema-init/dbus-broker')
    os.makedirs(os.path.dirname(svc))
    shutil.copy2(STOCK_SVC, svc)
    original = open(STOCK_SVC).read()
    env = dict(os.environ, SCHEMA_DBUS_FLIP_ROOT=root)

    def flip_cmd(*a):
        return subprocess.run([flip, *a], env=env, capture_output=True, text=True, timeout=30)

    def lines(path):
        return open(path).read().splitlines()

    try:
        print("-- preflight --")
        r = flip_cmd('check')
        check('check passes on this box', r.returncode == 0, r.stderr.strip())

        print("-- arm --")
        r = flip_cmd('arm')
        check('arm succeeds', r.returncode == 0, r.stderr.strip())
        armed = lines(svc)
        check('exec is the launcher', f'exec={run_sh}' in armed, str(armed))
        check('stock exec/args gone', not any(l.startswith('args=') or l == 'exec=/usr/bin/dbus-daemon' for l in armed))
        check('ready_path set', 'ready_path=/run/dbus/system_bus_socket' in armed)
        check('dep and knobs kept', all(l in armed for l in ('dep=sysprep', 'no_new_privs=0', 'needs_root=1')))
        check('stock backed up verbatim', open(stock).read() == original)
        check('gate created', os.path.exists(gate))
        check('state armed', flip_cmd('state').stdout.strip() == 'armed')

        r = flip_cmd('arm')
        check('re-arm keeps the stock backup', r.returncode == 0 and open(stock).read() == original)

        print("-- rollback --")
        r = flip_cmd('rollback')
        check('rollback succeeds', r.returncode == 0, r.stderr.strip())
        check('dbus.svc restored verbatim', open(svc).read() == original)
        check('gate removed', not os.path.exists(gate))
        check('state skipped', flip_cmd('state').stdout.strip() == 'skipped')

        print("-- failed preflight leaves the box alone --")
        os.remove(os.path.join(lib, 'dissect_policy.py'))
        r = flip_cmd('arm')
        check('arm refuses without the dissolver', r.returncode != 0, r.stderr.strip())
        check('dbus.svc untouched', open(svc).read() == original)
        check('no gate', not os.path.exists(gate))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f">> {sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
