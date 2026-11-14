#!/usr/bin/env python3
# audit 3.3: live uevents run in per-event workers. Drives schema-udev through
# its test feed (a unix datagram socket in place of netlink; refused as root).
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, 'schema-udev')

if os.geteuid() == 0:
    print('  SKIP  test_udev_workers: the test feed is refused as root')
    sys.exit(0)
if not os.access(BIN, os.X_OK):
    print('  SKIP  test_udev_workers: build ./schema-udev first')
    sys.exit(0)

RULES = '''ENV{TESTSLEEP}=="?*", PROGRAM="/bin/sleep $env{TESTSLEEP}"
ENV{TESTRUN}=="?*", RUN+="/bin/sleep $env{TESTRUN}"
'''

results = []


def secs(t, t0):
    return 'never' if t is None else f'{t - t0:.2f}s'


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))


class Daemon:
    def __init__(self, timeout_ms=180000):
        self.dir = tempfile.mkdtemp(prefix='schema-udev-w-')
        os.mkdir(os.path.join(self.dir, 'rules'))
        open(os.path.join(self.dir, 'rules', '90-test.rules'), 'w').write(RULES)
        self.feed = os.path.join(self.dir, 'feed')
        env = dict(os.environ, SCHEMA_UDEV_TEST_FEED=self.feed,
                   SCHEMA_UDEV_TEST_RULES=os.path.join(self.dir, 'rules'),
                   SCHEMA_UDEV_EVENT_TIMEOUT_MS=str(timeout_ms), SCHEMA_UDEV_SLOW_MS='0')
        self.p = subprocess.Popen([BIN], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.lines = []
        threading.Thread(target=self.pump, daemon=True).start()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.seq = 0
        ok = self.wait_for(lambda l: 'test feed on' in l, 10)
        if not ok:
            self.stop()
            raise RuntimeError('daemon never reported the test feed: ' + ' | '.join(l for _, l in self.lines[-5:]))

    def pump(self):
        for line in self.p.stderr:
            self.lines.append((time.monotonic(), line.rstrip('\n')))

    def wait_for(self, pred, secs):
        end = time.monotonic() + secs
        while True:
            for t, l in list(self.lines):
                if pred(l):
                    return t
            if time.monotonic() >= end:
                return None
            time.sleep(0.02)

    def send(self, action, devpath, **kv):
        self.seq += 1
        rec = [f'{action}@{devpath}', f'ACTION={action}', f'DEVPATH={devpath}', f'SEQNUM={self.seq}']
        kv.setdefault('SUBSYSTEM', 'test')
        rec += [f'{k}={v}' for k, v in kv.items()]
        self.sock.sendto(('\0'.join(rec) + '\0').encode(), self.feed)
        return time.monotonic()

    def done(self, action, devpath, secs):
        return self.wait_for(lambda l: f'ms {action} {devpath}' in l and '] slow ' in l, secs)

    def stop(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()
        self.sock.close()
        shutil.rmtree(self.dir, ignore_errors=True)


d = Daemon()
try:
    t0 = d.send('add', '/devices/t/a', TESTSLEEP='5')
    d.send('add', '/devices/t/b')
    tb = d.done('add', '/devices/t/b', 3)
    ta = d.done('add', '/devices/t/a', 10)
    check('unrelated_not_blocked', tb is not None and tb - t0 < 1.5 and ta is not None and ta - t0 >= 5,
          f'b={secs(tb, t0)} a={secs(ta, t0)}')

    d.send('add', '/devices/t/c', TESTSLEEP='2')
    d.send('change', '/devices/t/c')
    tadd = d.done('add', '/devices/t/c', 6)
    tchg = d.done('change', '/devices/t/c', 6)
    check('same_device_ordered', tadd is not None and tchg is not None and tchg >= tadd)

    d.send('remove', '/devices/u1/sdb', SUBSYSTEM='block', MAJOR='8', MINOR='16', TESTSLEEP='2')
    d.send('add', '/devices/u2/sdb', SUBSYSTEM='block', MAJOR='8', MINOR='16')
    trm = d.done('remove', '/devices/u1/sdb', 6)
    tad = d.done('add', '/devices/u2/sdb', 6)
    check('replug_waits', trm is not None and tad is not None and tad >= trm)

    d.send('add', '/devices/t/r', TESTRUN='2')
    tr = d.done('add', '/devices/t/r', 6)
    hit = [l for _, l in d.lines if 'ms add /devices/t/r' in l]
    ms = int(hit[-1].split('] slow ')[1].split('ms')[0]) if hit else 0
    check('run_waited', tr is not None and ms >= 2000, f'{ms}ms')
finally:
    d.stop()

d = Daemon(timeout_ms=2000)
try:
    t0 = d.send('add', '/devices/t/d', TESTSLEEP='20')
    tk = d.wait_for(lambda l: 'event TIMED OUT' in l and '/devices/t/d' in l, 5)
    check('idle_deadline_fires', tk is not None and tk - t0 < 4, secs(tk, t0))
    t1 = d.send('change', '/devices/t/d')
    tc = d.done('change', '/devices/t/d', 3)
    check('device_freed_after_timeout', tc is not None and tc - t1 < 1, secs(tc, t1))
finally:
    d.stop()

d = Daemon()
try:
    d.send('add', '/devices/t/e', TESTSLEEP='20')
    time.sleep(0.5)
    t0 = time.monotonic()
    d.p.send_signal(signal.SIGTERM)
    try:
        d.p.wait(timeout=8)
    except subprocess.TimeoutExpired:
        pass
    gone = d.p.poll() is not None
    check('term_with_hung_worker', gone and time.monotonic() - t0 < 3, f'{time.monotonic() - t0:.2f}s')
finally:
    d.stop()

print(f"{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
