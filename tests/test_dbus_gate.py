#!/usr/bin/env python3
"""The schema-dbus launchers use the broker only when the gate file exists.

Shipping the broker binary must not flip a box by itself: without
/etc/schema-init/dbus-broker both launchers take stock dbus-daemon.
Needs dbus-daemon installed; the armed session case also needs schema-dbus.

  ./tests/test_dbus_gate.py     exit 0 all pass, 1 any fail
"""
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSION = os.path.join(REPO, 'scripts', 'schema-dbus-session-run.sh')
SYSTEM = os.path.join(REPO, 'scripts', 'schema-dbus-run.sh')

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))


def session_bus_owner(gate):
    run = tempfile.mkdtemp(prefix='schema-dbus-gate-')
    env = dict(os.environ, XDG_RUNTIME_DIR=run, HOME=run, SCHEMA_DBUS_GATE=gate)
    try:
        out = os.path.join(run, 'procs')
        subprocess.run(
            [SESSION, 'sh', '-c', 'sleep 0.5; ps -o comm= --ppid $$ > "$0"', out],
            env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=15)
        return open(out).read().split()
    finally:
        shutil.rmtree(run, ignore_errors=True)


def main():
    tmp = tempfile.mkdtemp(prefix='schema-dbus-gate-')
    gate = os.path.join(tmp, 'dbus-broker')
    try:
        print("-- session launcher --")
        procs = session_bus_owner(gate)
        check('unarmed: stock dbus-daemon', 'dbus-daemon' in procs and 'schema-dbus' not in procs, str(procs))
        if shutil.which('schema-dbus') or os.path.exists('/usr/local/bin/schema-dbus'):
            open(gate, 'w').close()
            procs = session_bus_owner(gate)
            check('armed: schema-dbus', 'schema-dbus' in procs and 'dbus-daemon' not in procs, str(procs))
            os.remove(gate)
        else:
            print("  SKIP  armed: schema-dbus not installed")

        print("-- system launcher --")
        r = subprocess.run([SYSTEM], env=dict(os.environ, SCHEMA_DBUS_GATE=gate),
                           capture_output=True, text=True, timeout=15)
        check('unarmed: falls back before touching the broker',
              'broker not armed' in r.stderr, r.stderr.strip().splitlines()[0] if r.stderr else '')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f">> {sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
