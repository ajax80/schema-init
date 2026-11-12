#!/usr/bin/env python3
# The control socket from outside: schema-user runs PID 1's engine.c, so a client
# that misbehaves here would hold PID 1 the same way, with no root or VM needed.
import json, os, shutil, signal, socket, subprocess, sys, tempfile, threading, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CTL, USER = os.path.join(REPO, "schema-ctl"), os.path.join(REPO, "schema-user")
results = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  ok  " if cond else "  FAIL ") + name + ("" if cond else "  " + str(detail)))


if os.geteuid() == 0:
    print("test_ctl_socket: skipped (root)")
    sys.exit(0)

T = tempfile.mkdtemp(prefix="ctlsock.")
RUN, SVC = T + "/run", T + "/run/schema-user/services"
SOCK = RUN + "/schema-user/ctl.sock"
os.makedirs(SVC, 0o700)
os.chmod(RUN, 0o700)
ENV = dict(os.environ, XDG_RUNTIME_DIR=RUN, XDG_STATE_HOME=T + "/state", XDG_CONFIG_HOME=T + "/cfg",
           SCHEMA_USER_IMPORT="")
ENV.pop("SCHEMA_CTL_FALLBACK", None)
sl = T + "/sl"
os.symlink(shutil.which("sleep"), sl)


def svc(name, *lines):
    with open("%s/%s.svc" % (SVC, name), "w") as f:
        f.write("name=%s\n" % name + "\n".join(lines) + "\n")
    os.chmod("%s/%s.svc" % (SVC, name), 0o600)


def sleeper(name, *extra):
    svc(name, "exec=" + sl, "args=1000", "stable_secs=1", *extra)


# 68 services: enough small replies that an unread `status --kv` fills the socket
for i in range(66):
    sleeper("t%03d" % i)
sleeper("victim")
sleeper("held")
LONG = "/tmp/" + "a" * 490
sleeper("longpath", "protect_system=full", "rw_path=" + LONG)
COUNT = T + "/once.count"
# ignores SIGTERM, so a stop is still in progress when start_timeout_sec expires;
# ARMED says the trap is set (a SIGTERM before it kills the shell outright)
ARMED = T + "/slowstop.armed"
svc("slowstop", "exec=/bin/sh", "args=-c",
    'args=trap "" TERM; : > %s; while :; do %s 1; done' % (ARMED, shutil.which("sleep")),
    "notify=1", "start_timeout_sec=3", "stop_timeout_sec=60")
svc("once", "exec=/bin/sh", "args=-c", "args=echo x >> " + COUNT, "oneshot=1")
# quotes, control bytes and non-UTF-8 are refused at load now; backslash is
# allowed (systemd-escaped instance names), and a raw `\u` or `\x` copied into
# JSON unescaped is invalid, so `status --json` must still escape it
HOSTILE = 'q\\u00\\x2dend\\'
with open(SVC + "/jsonhostile.svc", "w", encoding="latin-1") as f:
    f.write('name=%s\nexec=%s\nargs=1000\nstable_secs=1\n' % (HOSTILE, sl))
os.chmod(SVC + "/jsonhostile.svc", 0o600)
OK = T + "/fails.ok"
svc("fails", "exec=/bin/sh", "args=-c",
    'args=[ -e %s ] && exec %s 1000; exit 3' % (OK, sl), "no_restart=1")


def ctl(*a, timeout=5, env=ENV):
    t0 = time.monotonic()
    try:
        # --kv replies carry raw name bytes (only --json promises clean parse):
        # a hostile name must fail the json check, not crash the harness here
        r = subprocess.run([CTL, "--user", *a], env=env, capture_output=True, text=True,
                           errors="replace", timeout=timeout)
        return r.returncode, r.stdout, r.stderr, time.monotonic() - t0
    except subprocess.TimeoutExpired:
        return "TIMEOUT", "", "", time.monotonic() - t0


def state(name):
    return ctl("status", name, timeout=10)[1].split("\n")[0]


# state() is "" when status fails: these stay false for a dead supervisor
def up(name):
    s = state(name)
    return "pid=" in s and "pid=0" not in s


def excised(name):
    return "state=EXCISED" in state(name)


def wait_for(cond, secs=8):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


def connect():
    c = socket.socket(socket.AF_UNIX)
    c.connect(SOCK)
    return c


def fake_server(reply):
    # schema-ctl against a server that replies with these exact bytes, then closes
    x = tempfile.mkdtemp(dir=T)
    os.makedirs(x + "/schema-user")
    s = socket.socket(socket.AF_UNIX)
    s.bind(x + "/schema-user/ctl.sock")
    s.listen(1)

    def serve():
        c, _ = s.accept()
        c.recv(256)
        c.sendall(reply)
        c.close()
    threading.Thread(target=serve, daemon=True).start()
    return ctl("status", env=dict(ENV, XDG_RUNTIME_DIR=x))


def excised_fails():
    return excised("fails")


log = open(T + "/log", "w")
sup = subprocess.Popen([USER], env=ENV, stdout=log, stderr=log, start_new_session=True)
try:
    check("socket appears", wait_for(lambda: os.path.exists(SOCK)))
    check("all services running", wait_for(lambda: up("t065") and up("held") and up("victim")))

    rc, out, _, _ = ctl("status", "--kv")
    check("whole --kv reply reaches a normal client", rc == 0 and "service.longpath.state=" in out, rc)

    rc, out, _, _ = ctl("status", "--json")
    try:
        names = [s["name"] for s in json.loads(out)["services"]]
        check("status --json stays parseable with a hostile name", HOSTILE in names, names[-3:])
    except ValueError as e:
        check("status --json stays parseable with a hostile name", False, (e, out[:160]))

    # a reader that never reads must not hold the supervisor
    stuck = connect()
    stuck.sendall(b"status --kv\n")
    time.sleep(0.3)
    rc, _, _, secs = ctl("list", timeout=4)
    check("stuck reader: another client is answered", rc == 0 and secs < 1, (rc, secs))
    stuck.close()
    check("answers normally after the stuck reader", ctl("list", timeout=4)[0] == 0)

    # one line at a time, never long enough to count as stuck
    trickle = connect()
    trickle.sendall(b"status --kv\n")
    done = threading.Event()

    def read_slowly():
        while not done.is_set():
            try:
                if not trickle.recv(200):
                    return
            except OSError:
                return
            time.sleep(0.4)
    tr = threading.Thread(target=read_slowly)
    tr.start()
    time.sleep(1.5)
    rc, _, _, secs = ctl("list", timeout=4)
    done.set()
    tr.join()
    trickle.close()
    check("slow reader: another client is answered", rc == 0 and secs < 1, (rc, secs))
    check("answers normally after the slow reader", ctl("list", timeout=8)[0] == 0)

    # reconnecting costs nothing per connection either
    t0 = time.monotonic()
    held_open = []
    for _ in range(30):
        c = connect()
        c.sendall(b"status --kv\n")
        held_open.append(c)
    secs = time.monotonic() - t0
    rc = ctl("list", timeout=4)[0]
    for c in held_open:
        c.close()
    check("30 stuck readers in a row take under 3 s", rc == 0 and secs < 3, (rc, secs))

    # slow sender: each byte inside the per-read timeout, 255 of them in all
    res = {}

    def probe():
        time.sleep(0.3)
        res["r"] = ctl("list", timeout=20)
    th = threading.Thread(target=probe)
    th.start()
    slow = connect()
    try:
        for b in b"status " + b"x" * 300:
            slow.send(bytes([b]))
            time.sleep(0.05)
            if "r" in res:
                break
    except OSError:
        pass
    th.join()
    slow.close()
    check("slow sender: another client waits under 2 s", res["r"][0] == 0 and res["r"][3] < 2, res["r"][::3])

    # a command cut off by the deadline is not a command: never act on half of it
    cut = connect()
    cut.sendall(b"stop victim")
    time.sleep(1.5)
    try:
        cut.sendall(b"-and-more\n")
    except OSError:
        pass
    cut.close()
    time.sleep(0.5)
    check("unfinished line is not executed", up("victim"), state("victim"))

    # nor when the client closes halfway through it
    cut = connect()
    cut.sendall(b"stop victim")
    cut.shutdown(socket.SHUT_WR)
    reply = cut.recv(200)
    cut.close()
    check("line ended by EOF is refused", reply.startswith(b"err: incomplete"), reply)
    time.sleep(0.5)
    check("line ended by EOF is not executed", up("victim"), state("victim"))

    # a line longer than ctl_writef's buffer stays inside it
    c = connect()
    c.sendall(b"status longpath\n")
    data = b""
    while True:
        d = c.recv(65536)
        if not d:
            break
        data += d
    c.close()
    check("long rw_path: reply is present", b"rw_path" in data, data[:80])
    check("long rw_path: no bytes from past the buffer", b"\0" not in data, data[-40:])
    check("long rw_path: no line over 511 bytes", max(len(l) + 1 for l in data.split(b"\n")) <= 511)
    check("long rw_path: next line still starts cleanly", data.endswith(b"\n.\n"), data[-20:])

    # the client reports a reply with no terminator
    rc, out, err, _ = fake_server(b"one\ntwo\n.\n")
    check("whole reply: exit 0, terminator stripped", rc == 0 and out == "one\ntwo\n", (rc, out))
    rc, out, err, _ = fake_server(b"one\ntw")
    check("cut reply: partial output kept", out == "one\ntw", out)
    check("cut reply: exit 1", rc == 1, rc)
    check("cut reply: says so", "cut short" in err, err)
    rc, out, err, _ = fake_server(b"path.\n")
    check("line ending in a dot is not the terminator", rc == 1 and "cut short" in err, (rc, err))
    rc, out, err, _ = fake_server(b"path.\n.\n")
    check("line ending in a dot, then the terminator", rc == 0 and out == "path.\n", (rc, out))
    rc, out, err, _ = fake_server(b"")
    check("empty reply: exit 1, no output", rc == 1 and out == "", (rc, out))

    # bare reset retries what failed and nothing else
    check("oneshot ran once", wait_for(lambda: os.path.exists(COUNT)))
    check("failing service excised", wait_for(excised_fails), state("fails"))
    check("failure is recorded", "excise_reason=exit-code" in ctl("status", "fails")[1], ctl("status", "fails")[1])
    ctl("stop", "held")
    check("stopped service is down", wait_for(lambda: excised("held")), state("held"))
    # a stop that start_timeout_sec overtook is still a stop
    check("slowstop timed out at boot", wait_for(lambda: excised("slowstop")), state("slowstop"))
    # the stop has to land inside the 2-3 s window (start_timeout_sec counts whole
    # seconds); a loaded runner can miss it, the timeout excises first and the
    # stop finds nothing running, so start over until a stop catches it running
    reply = ""
    for _ in range(5):
        if os.path.exists(ARMED):
            os.unlink(ARMED)
        ctl("start", "slowstop")
        if not wait_for(lambda: os.path.exists(ARMED)):
            break
        reply = ctl("stop", "slowstop")[1]
        if "SIGTERM" in reply:
            break
        wait_for(lambda: excised("slowstop"))
    check("slowstop: a stop caught it running", "SIGTERM" in reply, (reply, state("slowstop")))
    check("stop overtaken by the start timeout", "SIGTERM" in reply and wait_for(lambda: excised("slowstop") and
          "excise_reason=start-timeout" in ctl("status", "slowstop")[1]), ctl("status", "slowstop")[1])
    open(OK, "w").close()
    ctl("reset")
    check("reset: failed service comes back", wait_for(lambda: up("fails")), state("fails"))
    time.sleep(1)
    check("reset: stopped service stays down", excised("held"), state("held"))
    check("reset: stop overtaken by a timeout stays down", excised("slowstop"), state("slowstop"))
    check("reset: oneshot not re-run", open(COUNT).read() == "x\n", open(COUNT).read())
    check("reset: named service is still restartable", ctl("reset", "held")[0] == 0 and
          wait_for(lambda: up("held")), state("held"))
finally:
    sup.send_signal(signal.SIGTERM)
    try:
        sup.wait(15)
    except subprocess.TimeoutExpired:
        os.killpg(sup.pid, signal.SIGKILL)
    # any sleeper a killed supervisor left behind: argv[0] is unique to this run
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            with open("/proc/%s/cmdline" % pid, "rb") as f:
                if f.read().split(b"\0")[0] == sl.encode():
                    os.kill(int(pid), signal.SIGKILL)
        except OSError:
            pass
    log.close()
    shutil.rmtree(T, ignore_errors=True)

print("test_ctl_socket: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
