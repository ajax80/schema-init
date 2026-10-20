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

        print("-- system launcher stored fds --")
        probe = os.path.join(tmp, 'probe')
        with open(probe, 'w') as f:
            f.write('#!/bin/sh\necho "$0 LISTEN_FDS=${LISTEN_FDS-unset} LISTEN_PID=${LISTEN_PID-unset} '
                    'LISTEN_FDNAMES=${LISTEN_FDNAMES-unset} NOTIFY_SOCKET=${NOTIFY_SOCKET-unset}"\n'
                    'for f in /proc/self/fd/*; do n=${f##*/}; [ "$n" -gt 2 ] && [ "$(readlink "$f")" = /dev/null ] && echo "fd $n"; done\n')
        os.chmod(probe, 0o755)
        keep = [os.open('/dev/null', os.O_RDONLY) for _ in range(14)]
        stored = [fd for fd in keep if fd >= 3]
        senv = dict(os.environ, LISTEN_FDS='2', LISTEN_PID='1', LISTEN_FDNAMES='a:b', NOTIFY_SOCKET='/x')
        fdl = lambda out: sorted(int(l.split()[1]) for l in out.splitlines() if l.startswith('fd '))
        r = subprocess.run([SYSTEM], env=dict(senv, SCHEMA_DBUS_GATE=gate + '.absent', SCHEMA_DBUS_STOCK=probe),
                           pass_fds=stored, capture_output=True, text=True, timeout=15)
        check('fallback: stock daemon gets no LISTEN_*/NOTIFY_SOCKET',
              'LISTEN_FDS=unset LISTEN_PID=unset LISTEN_FDNAMES=unset NOTIFY_SOCKET=unset' in r.stdout, r.stdout.splitlines()[0] if r.stdout else r.stderr)
        check('fallback: stock daemon inherits no fds >= 3', fdl(r.stdout) == [] and 'fd ' not in r.stdout, str(fdl(r.stdout)))
        nopy = os.path.join(tmp, 'nopyfd')
        os.makedirs(nopy)
        for t in ('readlink', 'dirname', 'cat', 'sed', 'tr', 'ls'):
            src = shutil.which(t)
            if src:
                os.symlink(src, os.path.join(nopy, t))
        r = subprocess.run([SYSTEM], env=dict(senv, PATH=nopy, SCHEMA_DBUS_GATE=gate + '.absent', SCHEMA_DBUS_STOCK=probe),
                           pass_fds=stored, capture_output=True, text=True, timeout=15)
        check('fallback without python3: no fds >= 3, no LISTEN_*',
              'LISTEN_FDS=unset' in r.stdout and fdl(r.stdout) == [] and 'fd ' not in r.stdout, r.stdout + r.stderr)
        open(gate, 'w').close()
        r = subprocess.run([SYSTEM], env=dict(senv, SCHEMA_DBUS_GATE=gate, SCHEMA_DBUS_BROKER=probe,
                           SCHEMA_DBUS_CACHE=os.path.join(tmp, 'c2', 'x'), SCHEMA_DBUS_POLDIR=os.path.join(tmp, 'r2')),
                           pass_fds=stored, capture_output=True, text=True, timeout=30)
        if 'LISTEN_FDS=2' in r.stdout:
            check('broker path: variables untouched', 'LISTEN_PID=1 LISTEN_FDNAMES=a:b NOTIFY_SOCKET=/x' in r.stdout)
            check('broker path: stored fds kept', fdl(r.stdout) == sorted(stored), str(fdl(r.stdout)))
        else:
            print("  SKIP  broker path: dissolve unavailable")
        for fd in keep:
            os.close(fd)

        print("-- system launcher policy cache --")
        open(gate, 'w').close()
        broker = os.path.join(tmp, 'broker')
        with open(broker, 'w') as f:
            f.write('#!/bin/sh\necho "$SCHEMA_DBUS_POLICY"\n')
        os.chmod(broker, 0o755)
        cache = os.path.join(tmp, 'cache', 'dbus.policy')
        poldir = os.path.join(tmp, 'run')
        env = dict(os.environ, SCHEMA_DBUS_GATE=gate, SCHEMA_DBUS_BROKER=broker,
                   SCHEMA_DBUS_CACHE=cache, SCHEMA_DBUS_POLDIR=poldir)

        def launch(e=env):
            return subprocess.run([SYSTEM], env=e, capture_output=True, text=True, timeout=30).stdout.strip()

        if os.path.exists('/usr/share/dbus-1/system.conf'):
            first = launch()
            check('cold: dissolves into the run dir and fills the cache',
                  first == os.path.join(poldir, 'dbus.policy') and os.path.getsize(cache) > 0
                  and open(cache).read() == open(first).read(), first)
            nopy = os.path.join(tmp, 'nopyfd')
            os.mkdir(nopy)
            with open(os.path.join(nopy, 'python3'), 'w') as f:
                f.write('#!/bin/sh\nexit 1\n')
            os.chmod(os.path.join(nopy, 'python3'), 0o755)
            check('warm: broker reads the cache, python3 never runs',
                  launch(dict(env, PATH=nopy + ':' + os.environ['PATH'])) == cache)
            st = os.path.getmtime(cache)
            os.utime(broker, (st + 10, st + 10))
            check('broker newer than the cache: dissolves again', launch() == os.path.join(poldir, 'dbus.policy'))
            os.utime(cache, (0, 0))
            check('D-Bus config newer than the cache: dissolves again',
                  launch() == os.path.join(poldir, 'dbus.policy') and os.path.getmtime(cache) > 0)
        else:
            print("  SKIP  policy cache: no /usr/share/dbus-1/system.conf")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f">> {sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
