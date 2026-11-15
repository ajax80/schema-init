#!/usr/bin/env python3
# RUN helpers' stdout/stderr are discarded like udevd's (pktsetup on kernels
# without pktcdvd, hdparm chatter); SCHEMA_UDEV_RUN_OUTPUT=1 keeps them.
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, 'schema-udev')

if os.geteuid() == 0:
    print('  SKIP  test_udev_run_output: the test feed is refused as root')
    sys.exit(0)
if not os.access(BIN, os.X_OK):
    print('  SKIP  test_udev_run_output: build ./schema-udev first')
    sys.exit(0)

RULES = '''ENV{TESTRUN}=="1", RUN+="/bin/sh -c 'echo RUN-STDOUT-MARK; echo RUN-STDERR-MARK >&2; touch $env{TESTDONE}'"
ENV{TESTRUN}=="1", RUN+="/bin/sh -c 'exit 3'"
'''
results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))


def run(extra_env, workdir):
    d = tempfile.mkdtemp(prefix='schema-udev-o-', dir=workdir)
    os.mkdir(os.path.join(d, 'rules'))
    open(os.path.join(d, 'rules', '90-test.rules'), 'w').write(RULES)
    feed, done = os.path.join(d, 'feed'), os.path.join(d, 'done')
    env = dict(os.environ, SCHEMA_UDEV_TEST_FEED=feed, SCHEMA_UDEV_TEST_RULES=os.path.join(d, 'rules'))
    env.pop('SCHEMA_UDEV_RUN_OUTPUT', None)
    env.update(extra_env)
    p = subprocess.Popen([BIN], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    lines = []
    for f in (p.stdout, p.stderr):
        threading.Thread(target=lambda f=f: [lines.append(l) for l in f], daemon=True).start()
    try:
        end = time.monotonic() + 10
        while not any('test feed on' in l for l in lines) and time.monotonic() < end:
            time.sleep(0.02)
        if not any('test feed on' in l for l in lines):
            return False, ''.join(lines)
        rec = ['add@/devices/t/x', 'ACTION=add', 'DEVPATH=/devices/t/x', 'SEQNUM=1',
               'SUBSYSTEM=test', 'TESTRUN=1', f'TESTDONE={done}']
        socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM).sendto(('\0'.join(rec) + '\0').encode(), feed)
        end = time.monotonic() + 10
        while not os.path.exists(done) and time.monotonic() < end:
            time.sleep(0.02)
        time.sleep(0.3)
    finally:
        p.terminate()
        p.wait(5)
    time.sleep(0.1)
    return os.path.exists(done), ''.join(lines)


work = tempfile.mkdtemp(prefix='schema-udev-out-')
try:
    ran, out = run({}, work)
    check('default: RUN helper ran', ran)
    check('default: failing helper leaves one exit line', 'RUN exited 3' in out)
    check('default: helper stdout not in daemon output', ran and 'RUN-STDOUT-MARK' not in out)
    check('default: helper stderr not in daemon output', ran and 'RUN-STDERR-MARK' not in out)
    ran, out = run({'SCHEMA_UDEV_RUN_OUTPUT': '0'}, work)
    check('RUN_OUTPUT=0: output still discarded', ran and 'RUN-STDERR-MARK' not in out)
    ran, out = run({'SCHEMA_UDEV_RUN_OUTPUT': '1'}, work)
    check('RUN_OUTPUT=1: helper ran', ran)
    check('RUN_OUTPUT=1: helper stdout kept', 'RUN-STDOUT-MARK' in out)
    check('RUN_OUTPUT=1: helper stderr kept', 'RUN-STDERR-MARK' in out)
    ran, out = run({'SCHEMA_UDEV_RUN_OUTPUT': 'yes'}, work)
    check('RUN_OUTPUT=yes: helper stderr kept', ran and 'RUN-STDERR-MARK' in out)
finally:
    shutil.rmtree(work, ignore_errors=True)

sys.exit(0 if all(results) else 1)
