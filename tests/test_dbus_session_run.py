#!/usr/bin/env python3
"""schema-dbus-session-run.sh across session attempts and a slow or dead broker.

The v0.4.18 ISO VM black screen (2026-10-24): attempt 1's broker was slow,
the launcher fell back to dbus-daemon, the dead broker's watcher killed the
session, and attempt 1's reaper then killed its dbus-daemon, which unlinked
attempt 2's socket. Needs dbus-daemon and dbus-send.

  ./tests/test_dbus_session_run.py     exit 0 all pass, 1 any fail
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSION = os.path.join(REPO, 'scripts', 'schema-dbus-session-run.sh')

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))


def stub(run, name, body):
    path = os.path.join(run, name)
    with open(path, 'w') as f:
        f.write('#!/bin/sh\n' + body + '\n')
    os.chmod(path, 0o755)
    return path


def start(run, **extra):
    env = dict(os.environ, XDG_RUNTIME_DIR=run, HOME=run,
               SCHEMA_DBUS_GATE=os.path.join(run, 'no-gate'))
    env.update(extra)
    log = open(os.path.join(run, 'launcher.log'), 'a')
    return subprocess.Popen([SESSION, 'sleep', '60'], env=env, stdin=subprocess.DEVNULL,
                            stdout=log, stderr=log, start_new_session=True)


def bus_answers(run):
    r = subprocess.run(['dbus-send', '--bus=unix:path=' + os.path.join(run, 'bus'),
                        '--print-reply', '--dest=org.freedesktop.DBus', '/',
                        'org.freedesktop.DBus.ListNames'],
                       capture_output=True, timeout=5)
    return r.returncode == 0


def stop(p):
    try:
        os.killpg(p.pid, 15)
    except ProcessLookupError:
        pass


def prior_attempt_reaped_late(run):
    a1 = start(run)
    time.sleep(1.5)
    a2 = start(run)
    time.sleep(1.5)
    a1.kill()               # attempt 1's session dies; its reaper fires within ~1s
    a1.wait()               # reaped, as the autologin loop does; a zombie keeps /proc/<pid>
    time.sleep(3)
    ok = os.path.exists(os.path.join(run, 'bus')) and bus_answers(run)
    stop(a1)
    stop(a2)
    return ok


def slow_broker(run):
    broker = stub(run, 'slow-broker',
                  'sleep 3; exec dbus-daemon --session --nofork --address="unix:path=$SCHEMA_DBUS_SOCKET"')
    gate = os.path.join(run, 'gate')
    open(gate, 'w').close()
    a = start(run, SCHEMA_DBUS_BROKER=broker, SCHEMA_DBUS_GATE=gate)
    time.sleep(5)
    fell_back = 'falling back' in open(os.path.join(run, 'launcher.log')).read()
    ok = not fell_back and a.poll() is None and bus_answers(run)
    stop(a)
    return ok, f'fell_back={fell_back} alive={a.poll() is None}'


def dead_broker(run):
    broker = stub(run, 'dead-broker', 'exit 1')
    gate = os.path.join(run, 'gate')
    open(gate, 'w').close()
    a = start(run, SCHEMA_DBUS_BROKER=broker, SCHEMA_DBUS_GATE=gate)
    time.sleep(5)           # the broker watcher polls every 2s
    ok = a.poll() is None and bus_answers(run)
    stop(a)
    return ok, f'alive={a.poll() is None}'


def main():
    if not (shutil.which('dbus-daemon') and shutil.which('dbus-send')):
        print('SKIP: needs dbus-daemon and dbus-send')
        return 0
    for name, fn in [('prior attempt reaped late keeps our socket', prior_attempt_reaped_late),
                     ('slow broker is waited for, not raced', slow_broker),
                     ('dead broker: fallback session is not killed', dead_broker)]:
        run = tempfile.mkdtemp(prefix='schema-session-run-')
        try:
            r = fn(run)
            ok, detail = r if isinstance(r, tuple) else (r, '')
            check(name, ok, detail)
        finally:
            subprocess.run(['pkill', '-f', run], capture_output=True)
            shutil.rmtree(run, ignore_errors=True)
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
