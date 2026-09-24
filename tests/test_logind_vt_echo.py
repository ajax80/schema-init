#!/usr/bin/env python3
"""Session VT line discipline: flow control and echo disarmed, stale input dropped.

Drives Login1Session._disarm_vt_flow_control against a pty, never a real VT.

  ./tests/test_logind_vt_echo.py        exit 0 all pass, 1 any fail
"""
import os
import pty
import sys
import tempfile
import termios
import time
import types
import importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNDIR = tempfile.mkdtemp(prefix='schema-logind-test-')
for _d in ('sessions', 'seats', 'users'):
    os.makedirs(os.path.join(RUNDIR, _d), exist_ok=True)
os.environ['SCHEMA_LOGIND_RUN_DIR'] = RUNDIR

spec = importlib.util.spec_from_file_location(
    'schema_logind', os.path.join(REPO, 'scripts', 'schema-logind.py'))
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))


def main():
    master, slave = pty.openpty()
    os.write(master, b'typed on the desktop\n')
    L.Login1Session._disarm_vt_flow_control(types.SimpleNamespace(vtnr=1), slave)
    attrs = termios.tcgetattr(slave)
    check('IXON off', not attrs[0] & termios.IXON)
    check('ECHO off', not attrs[3] & termios.ECHO)
    check('ECHONL off', not attrs[3] & termios.ECHONL)
    os.set_blocking(slave, False)
    try:
        pending = os.read(slave, 4096)
    except BlockingIOError:
        pending = b''
    check('pending input flushed', pending == b'', repr(pending))
    os.set_blocking(master, False)
    try:
        os.read(master, 4096)
    except BlockingIOError:
        pass
    os.write(master, b'more typing\n')
    time.sleep(0.2)
    try:
        echoed = os.read(master, 4096)
    except BlockingIOError:
        echoed = b''
    check('new keystrokes not echoed to the console', echoed == b'', repr(echoed))
    print(f"\n{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
