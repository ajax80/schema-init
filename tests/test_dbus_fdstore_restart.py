#!/usr/bin/env python3
"""schema-dbus planned restart through the fdstore, with this harness standing
in for PID 1. The broker runs on a scratch socket with a scratch policy and
never touches the live bus. Clients speak the D-Bus wire protocol over raw
sockets.

  ./tests/test_dbus_fdstore_restart.py     exit 0 all pass, 1 any fail
"""
import array
import fcntl
import os
import select
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BROKER = os.path.join(REPO, 'schema-dbus')
POLICY = ("context = default\n"
          "allow = send_destination:*\n"
          "allow = send_type:signal\n"
          "allow = own:*\n")
DRIVER = ('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus')
CALL, RETURN, ERROR, SIGNAL = 1, 2, 3, 4


class _W:
    def __init__(self):
        self.b = bytearray()

    def align(self, n):
        self.b += b'\0' * (-len(self.b) % n)

    def y(self, v):
        self.b.append(v)

    def u(self, v):
        self.align(4)
        self.b += struct.pack('<I', v)

    def s(self, v):
        v = v.encode()
        self.u(len(v))
        self.b += v + b'\0'

    def g(self, v):
        v = v.encode()
        self.y(len(v))
        self.b += v + b'\0'

    def val(self, t, v):
        {'s': self.s, 'o': self.s, 'u': self.u, 'g': self.g}[t](v)


def marshal(mtype, serial, fields, sig='', args=(), flags=0):
    body = _W()
    for t, v in zip(sig, args):
        body.val(t, v)
    if sig:
        fields = fields + [(8, 'g', sig)]
    h = _W()
    h.b += struct.pack('<BBBBII', ord('l'), mtype, flags, 1, len(body.b), serial)
    h.u(0)
    start = len(h.b)
    for code, t, v in fields:
        h.align(8)
        h.y(code)
        h.g(t)
        h.val(t, v)
    struct.pack_into('<I', h.b, 12, len(h.b) - start)
    h.align(8)
    return bytes(h.b + body.b)


def _rd(b, p, t):
    if t in 'so':
        p += -p % 4
        n = struct.unpack_from('<I', b, p)[0]
        return b[p + 4:p + 4 + n].decode(), p + 5 + n
    if t == 'g':
        n = b[p]
        return b[p + 1:p + 1 + n].decode(), p + 2 + n
    if t in 'ub':
        p += -p % 4
        return struct.unpack_from('<I', b, p)[0], p + 4
    if t == 'y':
        return b[p], p + 1
    raise ValueError(t)


class Msg:
    def __init__(self, mtype, serial, fields, args):
        self.mtype, self.serial, self.fields, self.args = mtype, serial, fields, args
        self.member = fields.get(3)
        self.sender = fields.get(7)
        self.reply_serial = fields.get(5)


def parse(b):
    if len(b) < 16:
        return None
    blen, serial, flen = struct.unpack_from('<III', b, 4)
    hlen = 16 + flen + (-(16 + flen) % 8)
    total = hlen + blen
    if len(b) < total:
        return None
    fields, p = {}, 16
    while p < 16 + flen:
        p += -p % 8
        code = b[p]
        t, p = _rd(b, p + 1, 'g')
        fields[code], p = _rd(b, p, t)
    body, args, q = b[hlen:total], [], 0
    try:
        for t in fields.get(8, ''):
            v, q = _rd(body, q, t)
            args.append(v)
    except ValueError:
        pass
    return total, Msg(b[1], serial, fields, args)


class Client:
    def __init__(self, path, rcvbuf=None, hello=True):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.connect(path)
        self.buf, self.q, self.serial = b'', [], 0
        self.sock.sendall(b'\0AUTH EXTERNAL ' + str(os.getuid()).encode().hex().encode() + b'\r\n')
        while b'\r\n' not in self.buf:
            self._fill(5)
        line, self.buf = self.buf.split(b'\r\n', 1)
        assert line.startswith(b'OK'), line
        self.sock.sendall(b'BEGIN\r\n')
        self.unique = self.driver('Hello').args[0] if hello else None

    def _fill(self, timeout):
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            raise TimeoutError('no data')
        d = self.sock.recv(1 << 20)
        if not d:
            raise EOFError('broker closed the connection')
        self.buf += d

    def build(self, mtype, fields, sig='', args=(), flags=0):
        self.serial += 1
        return self.serial, marshal(mtype, self.serial, fields, sig, args, flags)

    def send(self, mtype, fields, sig='', args=(), flags=0):
        serial, data = self.build(mtype, fields, sig, args, flags)
        self.sock.sendall(data)
        return serial

    def recv(self, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            m = parse(self.buf)
            if m:
                self.buf = self.buf[m[0]:]
                return m[1]
            self._fill(max(0.0, deadline - time.monotonic()))

    def wait(self, pred, timeout=5):
        for i, m in enumerate(self.q):
            if pred(m):
                return self.q.pop(i)
        deadline = time.monotonic() + timeout
        while True:
            m = self.recv(max(0.0, deadline - time.monotonic()))
            if pred(m):
                return m
            self.q.append(m)

    def drain(self, timeout=0.3):
        try:
            while True:
                self.q.append(self.recv(timeout))
        except TimeoutError:
            pass

    def call(self, dest, path, iface, member, sig='', args=()):
        s = self.send(CALL, [(1, 'o', path), (2, 's', iface), (3, 's', member), (6, 's', dest)], sig, args)
        m = self.wait(lambda m: m.mtype in (RETURN, ERROR) and m.reply_serial == s)
        if m.mtype == ERROR:
            raise RuntimeError(f'{member}: {m.fields.get(4)} {m.args}')
        return m

    def driver(self, member, sig='', args=()):
        return self.call(DRIVER[0], DRIVER[1], DRIVER[2], member, sig, args)

    def signal(self, member, sig='', args=()):
        return self.send(SIGNAL, [(1, 'o', '/org/test'), (2, 's', 'org.test.Iface'), (3, 's', member)], sig, args)

    def close(self):
        self.sock.close()


class FakePid1:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
        self.sock.bind(path)
        self.path = path
        self.store, self.removed, self.datagrams = [], [], 0
        self.lock = threading.Lock()
        self.last = time.monotonic()
        self.stop = False
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        anc = socket.CMSG_SPACE(253 * 4) + socket.CMSG_SPACE(12)
        while not self.stop:
            r, _, _ = select.select([self.sock], [], [], 0.05)
            if not r:
                continue
            msg, cm, _, _ = self.sock.recvmsg(4096, anc, socket.MSG_CMSG_CLOEXEC)
            fds = []
            for level, typ, data in cm:
                if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                    a = array.array('i')
                    a.frombytes(data[:len(data) - len(data) % a.itemsize])
                    fds += list(a)
            kv = dict(l.split('=', 1) for l in msg.decode().split('\n') if '=' in l)
            with self.lock:
                self.datagrams += 1
                self.last = time.monotonic()
                name = kv.get('FDNAME', 'stored')
                if kv.get('FDSTORE') == '1':
                    self.store += [(name, fd) for fd in fds]
                    fds = []
                elif kv.get('FDSTOREREMOVE') == '1':
                    self.removed.append(name)
                    for n, fd in self.store:
                        if n == name:
                            os.close(fd)
                    self.store = [e for e in self.store if e[0] != name]
            for fd in fds:
                os.close(fd)

    def settle(self, quiet=0.2):
        while True:
            with self.lock:
                idle = time.monotonic() - self.last
            if idle >= quiet:
                return
            time.sleep(quiet - idle)

    def snapshot(self):
        with self.lock:
            return list(self.store)

    def names(self):
        return [n for n, _ in self.snapshot()]

    def replace(self, name, fd):
        with self.lock:
            for i, (n, old) in enumerate(self.store):
                if n == name:
                    os.close(old)
                    self.store[i] = (name, fd)
                    return

    def close(self):
        self.stop = True
        self.t.join()
        for _, fd in self.store:
            os.close(fd)
        self.sock.close()


class Broker:
    def __init__(self, t, fds=(), notify=True):
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
               'HOME': t.dir,
               'SCHEMA_DBUS_SOCKET': t.bus,
               'SCHEMA_DBUS_POLICY': t.policy,
               'SCHEMA_DBUS_SVCDIRS': t.svcdir,
               'SCHEMA_DBUS_MASKFILE': t.mask}
        if notify:
            env['NOTIFY_SOCKET'] = t.pid1.path
        self.log = os.path.join(t.dir, f'broker{t.nbroker}.log')
        t.nbroker += 1
        acts = [(os.POSIX_SPAWN_OPEN, 0, '/dev/null', os.O_RDONLY, 0),
                (os.POSIX_SPAWN_OPEN, 1, self.log, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644),
                (os.POSIX_SPAWN_DUP2, 1, 2)]
        highs = [fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 512) for _, fd in fds]
        for i, h in enumerate(highs):
            acts.append((os.POSIX_SPAWN_DUP2, h, 3 + i))
        if fds:
            env['LISTEN_FDS'] = str(len(fds))
            env['LISTEN_FDNAMES'] = ':'.join(n for n, _ in fds)
        argv = ['/bin/sh', '-c', 'export LISTEN_PID=$$; exec "$0"', BROKER]
        self.pid = os.posix_spawn('/bin/sh', argv, env, file_actions=acts)
        for h in highs:
            os.close(h)
        self.rc = None
        deadline = time.monotonic() + 5
        while 'listening on' not in self.output():
            if time.monotonic() > deadline or self.poll() is not None:
                raise RuntimeError('broker did not start: ' + self.output())
            time.sleep(0.02)

    def output(self):
        try:
            with open(self.log) as f:
                return f.read()
        except FileNotFoundError:
            return ''

    def poll(self):
        if self.rc is None:
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.rc = os.waitstatus_to_exitcode(st)
        return self.rc

    def wait(self, timeout):
        deadline = time.monotonic() + timeout
        while self.poll() is None:
            if time.monotonic() > deadline:
                return None
            time.sleep(0.01)
        return self.rc

    def kill(self):
        if self.poll() is None:
            os.kill(self.pid, signal.SIGKILL)
            self.wait(5)


class FdstoreRestart(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['make', '-s', 'schema-dbus'], cwd=REPO, check=True, capture_output=True)

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='sdbus-fds-')
        self.bus = os.path.join(self.dir, 'bus')
        self.policy = os.path.join(self.dir, 'policy')
        self.svcdir = os.path.join(self.dir, 'services')
        self.mask = os.path.join(self.dir, 'masked')
        os.mkdir(self.svcdir)
        with open(self.policy, 'w') as f:
            f.write(POLICY)
        open(self.mask, 'w').close()
        self.nbroker = 0
        self.clients = []
        self.pid1 = FakePid1(os.path.join(self.dir, 'notify'))
        self.broker = None

    def tearDown(self):
        for c in self.clients:
            c.close()
        if self.broker:
            self.broker.kill()
        self.pid1.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def start(self, **kw):
        self.broker = Broker(self, **kw)
        return self.broker

    def client(self, **kw):
        c = Client(self.bus, **kw)
        self.clients.append(c)
        return c

    def stop(self, limit=5):
        t0 = time.monotonic()
        os.kill(self.broker.pid, signal.SIGTERM)
        rc = self.broker.wait(limit)
        elapsed = time.monotonic() - t0
        self.assertEqual(rc, 0, self.broker.output())
        self.pid1.settle()
        return elapsed

    def resume(self):
        self.start(fds=self.pid1.snapshot())
        deadline = time.monotonic() + 5
        while self.pid1.snapshot() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(self.pid1.snapshot(), [], self.broker.output())

    def restart(self):
        self.stop()
        self.resume()

    @staticmethod
    def num(unique):
        return int(unique.rsplit('.', 1)[1])

    def test_restart_keeps_clients(self):
        self.start()
        a = self.client()
        self.assertEqual(a.driver('RequestName', 'su', ['org.test.A', 0]).args, [1])
        b = self.client()
        b.driver('AddMatch', 's', [f"type='signal',sender='{a.unique}'"])
        b.driver('AddMatch', 's', ["type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged'"])
        self.restart()
        a.signal('Ping', 's', ['hi'])
        m = b.wait(lambda m: m.mtype == SIGNAL and m.member == 'Ping')
        self.assertEqual((m.sender, m.args), (a.unique, ['hi']))
        self.assertEqual(b.driver('GetNameOwner', 's', ['org.test.A']).args, [a.unique])
        c = self.client()
        self.assertGreater(self.num(c.unique), max(self.num(a.unique), self.num(b.unique)))
        b.drain()
        noc = [m.args[0] for m in b.q if m.member == 'NameOwnerChanged']
        self.assertNotIn('org.test.A', noc)
        self.assertNotIn(a.unique, noc)

    def test_restart_mid_send(self):
        self.start()
        a, b = self.client(), self.client()
        serial, data = a.build(CALL, [(1, 'o', '/org/test'), (2, 's', 'org.test.Iface'),
                                      (3, 's', 'Big'), (6, 's', b.unique)], 's', ['x' * (200 * 1024)])
        half = len(data) // 2
        a.sock.sendall(data[:half])
        time.sleep(0.3)
        self.restart()
        a.sock.sendall(data[half:])
        m = b.wait(lambda m: m.mtype == CALL and m.member == 'Big')
        self.assertEqual(len(m.args[0]), 200 * 1024)
        b.send(RETURN, [(5, 'u', m.serial), (6, 's', m.sender)], 's', ['ok'])
        r = a.wait(lambda r: r.mtype in (RETURN, ERROR) and r.reply_serial == serial)
        self.assertEqual((r.mtype, r.args), (RETURN, ['ok']))

    def test_restart_stuck_reader(self):
        self.start()
        a, b = self.client(), self.client()
        b.driver('AddMatch', 's', [f"type='signal',sender='{a.unique}'"])
        d = self.client(rcvbuf=4096)
        d.driver('AddMatch', 's', [f"type='signal',sender='{b.unique}'"])
        blob = 'y' * 32768
        for _ in range(200):
            b.signal('Flood', 's', [blob])
        b.driver('GetId')
        elapsed = self.stop()
        self.assertLess(elapsed, 2.5)
        self.assertEqual(self.pid1.names().count('conn'), 2, self.broker.output())
        self.resume()
        d.sock.settimeout(5)
        while d.sock.recv(1 << 20):
            pass
        a.signal('Ping', 's', ['hi'])
        m = b.wait(lambda m: m.mtype == SIGNAL and m.member == 'Ping')
        self.assertEqual(m.sender, a.unique)
        self.assertEqual(b.driver('GetNameOwner', 's', [a.unique]).args, [a.unique])

    def test_store_removed_after_resume(self):
        self.start()
        self.client()
        self.stop()
        self.assertEqual(set(self.pid1.names()), {'listen', 'state', 'conn'})
        self.resume()
        self.pid1.settle()
        self.assertTrue({'listen', 'state', 'conn', 'pfd'} <= set(self.pid1.removed), self.pid1.removed)
        self.assertEqual(self.pid1.snapshot(), [])

    def test_no_notify_socket(self):
        self.start(notify=False)
        self.client()
        elapsed = self.stop()
        self.assertLess(elapsed, 1.5)
        self.assertEqual(self.pid1.datagrams, 0)
        self.assertEqual(self.pid1.snapshot(), [])

    def test_bad_state_starts_fresh(self):
        self.start()
        a, b = self.client(), self.client()
        self.stop()
        state = dict(self.pid1.snapshot())['state']
        st = os.fstat(state)
        blob = bytearray(os.pread(state, st.st_size, 0))
        struct.pack_into('=I', blob, 4, 99)
        bad = os.memfd_create('bad-state', os.MFD_CLOEXEC)
        os.write(bad, bytes(blob))
        self.pid1.replace('state', bad)
        self.resume()
        for c in (a, b):
            with self.assertRaises(EOFError):
                c.recv(5)
        c = self.client()
        self.assertTrue(c.unique.startswith(':1.'))
        self.assertIsNone(self.broker.poll())


if __name__ == '__main__':
    unittest.main()
