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
import signal
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


def bus_answers(run, within=20):
    deadline = time.time() + within
    while time.time() < deadline:
        r = subprocess.run(['dbus-send', '--bus=unix:path=' + os.path.join(run, 'bus'),
                            '--print-reply', '--dest=org.freedesktop.DBus', '/',
                            'org.freedesktop.DBus.ListNames'],
                           capture_output=True, timeout=10)
        if r.returncode == 0:
            return True
        time.sleep(0.2)
    return False


def stop(p):
    try:
        os.killpg(p.pid, 15)
    except ProcessLookupError:
        pass


def group(p, comm):
    r = subprocess.run(['pgrep', '-g', str(p.pid), '-x', comm], capture_output=True, text=True)
    return r.stdout.split()


def signal_all(pids, sig):
    for pid in pids:
        os.kill(int(pid), sig)


def dead_session_bus_stopped(run):
    a1 = start(run)
    bus_answers(run)
    # Hold attempt 1's reaper (the launcher's forked subshells) so it fires
    # after attempt 2 binds, as it did in the VM.
    reapers = group(a1, os.path.basename(SESSION)[:15])
    signal_all(reapers, signal.SIGSTOP)
    a1.kill()
    a1.wait()               # reaped, as the autologin loop does; a zombie keeps /proc/<pid>
    a2 = start(run)
    bus_answers(run)
    signal_all(reapers, signal.SIGCONT)
    time.sleep(3)
    ok = os.path.exists(os.path.join(run, 'bus')) and bus_answers(run, within=1)
    stop(a1)
    stop(a2)
    return ok


def live_session_bus_kept(run):
    a1 = start(run)
    bus_answers(run)
    first = group(a1, 'dbus-daemon')[0]
    a2 = start(run)
    time.sleep(2)
    ok = os.path.isdir('/proc/' + first)
    stop(a1)
    stop(a2)
    return ok


def slow_broker(run):
    broker = stub(run, 'slow-broker',
                  'sleep 3; exec dbus-daemon --session --nofork --address="unix:path=$SCHEMA_DBUS_SOCKET"')
    gate = os.path.join(run, 'gate')
    open(gate, 'w').close()
    a = start(run, SCHEMA_DBUS_BROKER=broker, SCHEMA_DBUS_GATE=gate)
    answers = bus_answers(run)
    fell_back = 'falling back' in open(os.path.join(run, 'launcher.log')).read()
    alive = a.poll() is None
    stop(a)
    return answers and alive and not fell_back, f'fell_back={fell_back} alive={alive}'


def dead_broker(run):
    broker = stub(run, 'dead-broker', 'exit 1')
    gate = os.path.join(run, 'gate')
    open(gate, 'w').close()
    a = start(run, SCHEMA_DBUS_BROKER=broker, SCHEMA_DBUS_GATE=gate)
    answers = bus_answers(run)
    time.sleep(5)           # the broker watcher polls every 2s
    alive = a.poll() is None
    stop(a)
    return answers and alive, f'alive={alive}'


def main():
    if not (shutil.which('dbus-daemon') and shutil.which('dbus-send')):
        print('SKIP: needs dbus-daemon and dbus-send')
        return 0
    for name, fn in [('dead session: its live bus is stopped before we bind', dead_session_bus_stopped),
                     ('live session: a second run leaves its bus alone', live_session_bus_kept),
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
