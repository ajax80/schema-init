#!/usr/bin/env python3
# The native socket is 0666 and the sink single-threaded: a passed pipe with a
# live writer used to block it in read() forever (audit 2026-11-12, 1.2).
# Refused fds must not stall it; a memfd or tmpfs file must still be read.
import fcntl, os, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SINK = os.path.join(HERE, "..", "schema-journal-sink")
T = tempfile.mkdtemp(prefix="schema-jsink-")
OUT = T + "/out"
for d in ("dev", "run"):
    os.mkdir(os.path.join(T, d))
env = dict(os.environ, JOURNAL_SINK_ROOT=T, JOURNAL_SINK_FILE=OUT)
env.pop("JOURNAL_SINK_KMSG", None)
p = subprocess.Popen([SINK], env=env, stderr=subprocess.DEVNULL)
NATIVE = T + "/run/systemd/journal/socket"
fails = 0


def check(name, ok):
    global fails
    print("  %s  %s" % ("ok" if ok else "FAIL", name))
    fails += not ok


def logged(tag, within=1.0):
    end = time.monotonic() + within
    while time.monotonic() < end:
        try:
            if tag in open(OUT).read():
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.02)
    return False


def send(payload=b"", fd=None):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    if fd is None:
        s.sendto(payload, NATIVE)
    else:
        s.connect(NATIVE)
        socket.send_fds(s, [payload], [fd])
    s.close()


try:
    for _ in range(250):
        if os.path.exists(NATIVE):
            break
        time.sleep(0.02)
    else:
        print("  FAIL  sink never bound %s (rc=%s)" % (NATIVE, p.poll()))
        raise SystemExit(1)
    rd, wr = os.pipe()   # wr stays open: a read() on rd never returns
    send(fd=rd)
    send(b"MESSAGE=after-pipe\n")
    check("a datagram after a passed pipe still lands within 1 s", logged("after-pipe"))

    m = os.memfd_create("j", os.MFD_ALLOW_SEALING)
    os.write(m, b"MESSAGE=via-memfd\n")
    fcntl.fcntl(m, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_WRITE)
    send(fd=m)
    check("a sealed memfd is read", logged("via-memfd"))

    u = os.memfd_create("j", os.MFD_ALLOW_SEALING)
    os.write(u, b"MESSAGE=via-unsealed\n")
    send(fd=u)
    send(b"MESSAGE=after-unsealed\n")
    check("an unsealed memfd is refused", logged("after-unsealed") and not logged("via-unsealed", 0.1))

    if os.statvfs("/dev/shm") and open("/proc/self/mounts").read().count(" /dev/shm tmpfs "):
        with tempfile.NamedTemporaryFile(dir="/dev/shm") as tf:
            tf.write(b"MESSAGE=via-tmpfs\n"); tf.flush()
            send(fd=tf.fileno())
            check("a regular file on tmpfs is read", logged("via-tmpfs"))

    f = open(T + "/disk", "wb+")   # T is not tmpfs on every box, so only refusal is safe to assert off tmpfs
    f.write(b"MESSAGE=via-sock-dir\n"); f.flush()
    send(fd=f.fileno())
    send(b"MESSAGE=after-file\n")
    check("the sink keeps running after a passed file", logged("after-file"))
finally:
    p.kill()
    p.wait()
    subprocess.run(["rm", "-rf", T])
print("PASS" if not fails else "FAIL")
sys.exit(1 if fails else 0)
